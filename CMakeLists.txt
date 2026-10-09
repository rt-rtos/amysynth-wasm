# hostsim: the firmware's engine and UI layer (synth_ui, the display
# renderers, main/input_dispatch.c, the harness interpreter) in one
# single-threaded host program (hostsim/README.md), built natively or with
# Emscripten into the static site. build.sh is the entry point: it
# configures this project into out/<mode>/build (emcmake for wasm) and builds
# it with Ninja.
cmake_minimum_required(VERSION 3.28)
project(hostsim C)

set(HOSTSIM_MODE native CACHE STRING "native, san or wasm")
set_property(CACHE HOSTSIM_MODE PROPERTY STRINGS native san wasm)
set(FW_ROOT "${CMAKE_SOURCE_DIR}/firmware" CACHE PATH "the firmware checkout to build")
set(HOSTSIM_EXTRA_FLAGS "$ENV{HOSTSIM_CFLAGS}" CACHE STRING "appended to the compile and link flags")
set(HOSTSIM_WASM_STACK 1MB CACHE STRING "wasm shadow stack (-sSTACK_SIZE)")
if(NOT HOSTSIM_MODE MATCHES "^(native|san|wasm)$")
    message(FATAL_ERROR "HOSTSIM_MODE: native, san or wasm, not '${HOSTSIM_MODE}'")
endif()
if((HOSTSIM_MODE STREQUAL wasm) AND NOT EMSCRIPTEN)
    message(FATAL_ERROR "wasm: configure with emcmake")
endif()

# Every flag comes from this file: none from the environment or a build type.
set(CMAKE_C_FLAGS "")
set(CMAKE_EXE_LINKER_FLAGS "")
set(CMAKE_BUILD_TYPE "")

set(R ${CMAKE_SOURCE_DIR})
file(REAL_PATH "${FW_ROOT}" FW)
set(OUT ${R}/out/${HOSTSIM_MODE})
set(B ${CMAKE_BINARY_DIR})
set(H ${R}/hostsim)
set(HOST ${R}/host)
set(SC ${FW}/components/synth_core)
set(DISP ${FW}/components/display)
set(U8 ${FW}/components/u8g2/csrc)
if(NOT EXISTS ${SC}/synth_ui/synth_ui_task.c)
    message(FATAL_ERROR "no firmware at ${FW} (git submodule update --init --recursive)")
endif()

# AMY as the firmware vendors it, at the device's 48 kHz: the host build
# takes amy.h's generic 44100 fallback, so the copy patches it. The copy
# mirrors the source directory and is redone when any of its files changes.
set(AMY_SRC ${FW}/components/amy/src)
set(AMY ${B}/amy)
file(GLOB_RECURSE AMY_FILES CONFIGURE_DEPENDS RELATIVE ${AMY_SRC} ${AMY_SRC}/*)
list(TRANSFORM AMY_FILES PREPEND ${AMY_SRC}/ OUTPUT_VARIABLE AMY_ORIG)
list(TRANSFORM AMY_FILES PREPEND ${AMY}/ OUTPUT_VARIABLE AMY_COPY)
add_custom_command(OUTPUT ${AMY_COPY}
    COMMAND sh ${R}/cmake/copy-amy.sh ${AMY_SRC} ${AMY}
    DEPENDS ${AMY_ORIG} ${R}/cmake/copy-amy.sh
    COMMENT "Copying AMY at 48 kHz"
    VERBATIM)

# The drum sample banks, from the samples of the AMY release the firmware
# pins, checked against the firmware's map of them.
set(DRUMS ${B}/gen/drums.bin)
add_custom_command(OUTPUT ${DRUMS}
    COMMAND python3 ${R}/tools/gen-drums.py ${FW} ${DRUMS} > /dev/null
    DEPENDS ${AMY_SRC}/pcm_gamma9001.h ${R}/tools/gen-drums.py
    COMMENT "Generating drums.bin"
    VERBATIM)
add_custom_target(drums ALL DEPENDS ${DRUMS})
if(HOSTSIM_MODE STREQUAL wasm)
    set(DRUMS_AT /components/amy/drums.bin)
else()
    set(DRUMS_AT ${DRUMS})
endif()

# The template table firmware builds generate (synth_core CMakeLists.txt).
# The generator parses the firmware's C headers, so every header it could
# read is an input. The table is replaced only when its content changed, so
# a header touch that leaves it the same recompiles only that header's
# includers (restat).
set(TPL_DIR ${SC}/project/templates)
set(TPL_C ${B}/gen/project_templates_data.c)
file(GLOB TPL_PY CONFIGURE_DEPENDS ${TPL_DIR}/*.py)
file(GLOB_RECURSE TPL_HDRS CONFIGURE_DEPENDS
    ${SC}/*.h ${DISP}/*.h ${AMY_SRC}/*.h ${FW}/components/project_store/include/*.h)
add_custom_command(OUTPUT ${TPL_C}
    COMMAND python3 gen_templates.py --check-src ${FW}/components --out ${TPL_C}.tmp > /dev/null
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TPL_C}.tmp ${TPL_C}
    WORKING_DIRECTORY ${TPL_DIR}
    DEPENDS ${TPL_PY} ${TPL_HDRS}
    COMMENT "Generating the template table"
    VERBATIM)

# stubs/ stands in for the ESP-IDF, FreeRTOS and driver headers the firmware
# includes (SHIMS.md). CF is the whole set in order, as the wasm links take
# it; the compiles take its parts through the target properties below.
set(CF_OPT -O2 -g -std=gnu11)
set(CF_DEF AMY_WAVETABLE GAMMA9001 AMY_USE_FIXEDPOINT= HOSTSIM
    MALLOC_CAP_SPIRAM=0 MALLOC_CAP_8BIT=0 HOSTSIM_DRUMS="${DRUMS_AT}")
set(CF_INC ${R}/stubs ${HOST} ${R}/config ${AMY} ${SC} ${SC}/include
    ${SC}/sequencer_core ${SC}/project ${SC}/synth_ui ${DISP}
    ${FW}/components/seq_clamp ${FW}/components/project_store/include
    ${FW}/components/usb_audio/include ${FW}/components/wireless/include
    ${FW}/components/diagnostics/include ${FW}/components/my_buttons/include
    ${FW}/components/harness/include ${FW}/components/project_xfer/include
    ${FW}/main ${U8})
# Source paths in assert messages (__FILE__) ship in the wasm: keep them
# relative to the firmware or to this repo. The first map covers the AMY
# copy and the generated table, which live in the build directory.
set(CF_MODE)
if(HOSTSIM_MODE STREQUAL wasm)
    list(APPEND CF_MODE -g0 -ffile-prefix-map=${B}/= -ffile-prefix-map=${FW}/=
                        -ffile-prefix-map=${R}/=)
elseif(HOSTSIM_MODE STREQUAL san)
    list(APPEND CF_MODE -fsanitize=address,undefined -fno-sanitize=shift
                        -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
endif()
separate_arguments(EXTRA UNIX_COMMAND "${HOSTSIM_EXTRA_FLAGS}")
list(TRANSFORM CF_DEF PREPEND -D OUTPUT_VARIABLE CF_DEF_FLAGS)
list(TRANSFORM CF_INC PREPEND -I OUTPUT_VARIABLE CF_INC_FLAGS)
set(CF ${CF_OPT} ${CF_DEF_FLAGS} ${CF_INC_FLAGS} ${CF_MODE} ${EXTRA})

# Firmware sources compile with the warning set of the device build (its
# compile commands), without -Werror. This repo's own sources take
# -Wall -Wextra.
set(FW_WARN -Wall -Wextra -Wno-enum-conversion -Wno-sign-compare -Wno-unused-parameter)
if(CMAKE_C_COMPILER_ID STREQUAL GNU)
    list(APPEND FW_WARN -Wno-old-style-declaration)
endif()
set(OWN_WARN -Wall -Wextra)

# Every AMY source but its standalone programs and other platforms' I/O.
# Under Emscripten AMY would take its own web build (AudioWorklet hooks, a
# wasm-worker lock). Compiled without __EMSCRIPTEN__ it takes the same path
# as the native host build; its audio backend is left out (audio is off).
set(AMY_SKIP amy-example amy-message amy-piano i2s linux_midi pyamy usb)
set(AMY_CF)
if(HOSTSIM_MODE STREQUAL wasm)
    list(APPEND AMY_SKIP libminiaudio-audio)
    set(AMY_CF -U__EMSCRIPTEN__ -DAMY_NO_MINIAUDIO -DAMY_HOST_MIDI)
endif()
file(GLOB AMY_TOP CONFIGURE_DEPENDS RELATIVE ${AMY_SRC} ${AMY_SRC}/*.c)
set(AMY_SRCS)
foreach(f IN LISTS AMY_TOP)
    string(REGEX REPLACE "\\.c$" "" m ${f})
    if(NOT m IN_LIST AMY_SKIP)
        list(APPEND AMY_SRCS ${AMY}/${f})
    endif()
endforeach()
set_source_files_properties(${AMY_SRCS} PROPERTIES COMPILE_OPTIONS "${FW_WARN};${AMY_CF}")

# The engine: every synth_core source outside synth_ui/ (compiled with the UI
# below) but amy_helpers.c, which host_pump.c compiles; plus the project
# store from its own component and the generated template table.
file(GLOB_RECURSE SC_ALL CONFIGURE_DEPENDS RELATIVE ${SC} ${SC}/*.c)
list(FILTER SC_ALL EXCLUDE REGEX "^synth_ui/")
list(REMOVE_ITEM SC_ALL amy_helpers.c)
list(TRANSFORM SC_ALL PREPEND ${SC}/ OUTPUT_VARIABLE SC_SRCS)
list(APPEND SC_SRCS ${FW}/components/project_store/project_store.c
                    ${FW}/components/project_store/project_tlv.c ${TPL_C})

# UI: every synth_ui source, the display renderers without the panel driver
# and its flush (hostsim keeps the buffer instead), the dispatcher, the
# harness interpreter, the project-transfer core (its UART0 transport,
# project_xfer_uart.c, is the device's), U8g2.
file(GLOB UI_SRCS CONFIGURE_DEPENDS ${SC}/synth_ui/*.c)
file(GLOB DISP_SRCS CONFIGURE_DEPENDS ${DISP}/*.c)
list(REMOVE_ITEM DISP_SRCS ${DISP}/priv_i2c_u8g2.c ${DISP}/display_flush.c
                           ${DISP}/display_flush_runs.c)
set(HARNESS_C ${FW}/components/harness/harness_exec.c)
list(APPEND UI_SRCS ${DISP_SRCS} ${FW}/main/input_dispatch.c
    ${FW}/components/usb_audio/usb_audio_watchdog.c ${HARNESS_C}
    ${FW}/components/project_xfer/project_xfer.c)
file(GLOB U8_SRCS CONFIGURE_DEPENDS ${U8}/*.c)
set_source_files_properties(${SC_SRCS} ${UI_SRCS} ${U8_SRCS}
    PROPERTIES COMPILE_OPTIONS "${FW_WARN}")
set_source_files_properties(${HARNESS_C}
    PROPERTIES COMPILE_OPTIONS "${FW_WARN};-DCONFIG_DEV_SERIAL_HARNESS=1")

# This repo's host side. host_pump.c compiles amy_helpers.c, so it takes the
# firmware's warning set. frame_out.c writes the replays' PNG and text frames.
set(HOST_SRCS ${HOST}/host_glue.c ${HOST}/host_pump.c)
if(NOT HOSTSIM_MODE STREQUAL wasm)
    list(APPEND HOST_SRCS ${HOST}/frame_out.c)
endif()
file(GLOB HS_SRCS CONFIGURE_DEPENDS ${H}/*.c)
set_source_files_properties(${HOST}/host_glue.c ${HOST}/frame_out.c
    PROPERTIES COMPILE_OPTIONS "${OWN_WARN}")
set_source_files_properties(${HOST}/host_pump.c PROPERTIES COMPILE_OPTIONS "${FW_WARN}")
set_source_files_properties(${HS_SRCS}
    PROPERTIES COMPILE_OPTIONS "${OWN_WARN};-DCONFIG_DEV_SERIAL_HARNESS=1")

# One set of objects for every link of the mode, in the old link order.
add_library(hs_obj OBJECT ${AMY_SRCS} ${SC_SRCS} ${UI_SRCS} ${U8_SRCS} ${HOST_SRCS} ${HS_SRCS})
target_compile_options(hs_obj PRIVATE ${CF_OPT} ${CF_MODE} ${EXTRA})
target_compile_definitions(hs_obj PRIVATE ${CF_DEF})
target_include_directories(hs_obj PRIVATE ${CF_INC})

if(NOT HOSTSIM_MODE STREQUAL wasm)
    add_executable(hostsim $<TARGET_OBJECTS:hs_obj>)
    set_target_properties(hostsim PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT} LINKER_LANGUAGE C)
    target_link_options(hostsim PRIVATE ${CF_OPT} ${CF_MODE} ${EXTRA} -pthread)
    target_link_libraries(hostsim PRIVATE z m)
    return()
endif()

# The static site: the module, its preloaded data, the page. Links run in
# their output directory so the loader names its data file without a path.
set(PRELOAD --preload-file ${DRUMS}@${DRUMS_AT})
set(LF -lm -sMODULARIZE=1 -sEXPORT_NAME=createHostsim -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=64MB -sSTACK_SIZE=${HOSTSIM_WASM_STACK} --no-entry
    -sINCOMING_MODULE_JS_API=wasmBinary,getPreloadedPackage,locateFile,print,printErr
    -sEXPORTED_FUNCTIONS=_hs_boot,_hs_step,_hs_json,_hs_json_len,_hs_frame,_hs_frame_len,_hs_audio,_hs_audio_len,_malloc,_free)
set(RT FS,HEAPU8,HEAP16,UTF8ToString,stringToNewUTF8)
# hs_link(<dir> <outputs> <flags...>): link the module into <dir>/hostsim.js.
function(hs_link dir outputs)
    list(TRANSFORM outputs PREPEND ${dir}/)
    add_custom_command(OUTPUT ${outputs}
        COMMAND ${CMAKE_C_COMPILER} ${CF} $<TARGET_OBJECTS:hs_obj> ${LF} ${ARGN} -o hostsim.js
        WORKING_DIRECTORY ${dir}
        DEPENDS $<TARGET_OBJECTS:hs_obj> ${DRUMS}
        COMMENT "Linking ${dir}/hostsim.js"
        COMMAND_EXPAND_LISTS VERBATIM)
endfunction()
set(MODULE_FILES hostsim.js hostsim.wasm hostsim.data)
hs_link(${OUT}/site "${MODULE_FILES}" ${PRELOAD} -lidbfs.js
    -sEXPORTED_RUNTIME_METHODS=${RT},IDBFS -sENVIRONMENT=web)
# The same module for node (hostsim/replay-wasm.mjs), and a checking build:
# wasm assertions, every load and store bounds-checked, stack overflow
# detection.
hs_link(${OUT}/node "${MODULE_FILES}" ${PRELOAD}
    -sEXPORTED_RUNTIME_METHODS=${RT} -sENVIRONMENT=node)
hs_link(${OUT}/node-check "${MODULE_FILES}" ${PRELOAD}
    -sEXPORTED_RUNTIME_METHODS=${RT} -sENVIRONMENT=node
    -sASSERTIONS=2 -sSAFE_HEAP=1 -sSTACK_OVERFLOW_CHECK=2 --profiling-funcs)
add_custom_target(modules ALL DEPENDS ${OUT}/node/hostsim.js ${OUT}/node-check/hostsim.js)

# The page and what goes beside it, on every build after the web module
# links, so the version it names is always the firmware tree's current one.
get_filename_component(EM ${CMAKE_C_COMPILER} DIRECTORY)
add_custom_target(site ALL
    COMMAND bash ${R}/tools/assemble-site.sh site ${R} ${FW} ${OUT} ${EM}
    DEPENDS ${OUT}/site/hostsim.js
    COMMENT "Assembling ${OUT}/site"
    VERBATIM)

# --single (target single, not built by default): the module carries the
# wasm and drums.bin inside its JS; the page carries its scripts,
# CONTROLS.md and the notices.
hs_link(${B}/single hostsim.js --embed-file ${DRUMS}@${DRUMS_AT} -lidbfs.js
    -sEXPORTED_RUNTIME_METHODS=${RT},IDBFS -sENVIRONMENT=web -sSINGLE_FILE=1
    -sSINGLE_FILE_BINARY_ENCODE=0 -Wno-unused-command-line-argument)
add_custom_target(single
    COMMAND bash ${R}/tools/assemble-site.sh single ${R} ${FW} ${OUT} ${B}/single/hostsim.js
    DEPENDS ${B}/single/hostsim.js
    COMMENT "Writing the single-file page"
    VERBATIM)
add_dependencies(single site)
