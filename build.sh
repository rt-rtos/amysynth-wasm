#!/usr/bin/env bash
# hostsim: the firmware's engine and UI layer (synth_ui, the display
# renderers, main/input_dispatch.c, the harness interpreter) in one
# single-threaded host program that advances only when told to
# (hostsim/README.md). Built natively, or with Emscripten into the static
# site that runs the device in a browser.
#
# Usage: ./build.sh [native|san|wasm] [--single]
#   native  out/native/hostsim (default)
#   san     the same with ASan and UBSan, out/san/hostsim
#   wasm    out/wasm/site, the static site, plus out/wasm/node and
#           out/wasm/node-check (the module for node, and a checking build)
#           Needs emcc on PATH (source emsdk_env.sh).
#   --single  with wasm: also out/wasm/S3-Amysynth-<version>.html, the whole
#           site in one file that opens from disk (file://)
#
# The firmware sources come from the firmware submodule; FW_ROOT points the
# build at another checkout (a working tree with uncommitted changes). The
# version shown in the page is `git describe` of that tree, or HOSTSIM_VERSION.
# Rebuilds only changed sources; HOSTSIM_CLEAN=1 starts over. HOSTSIM_CFLAGS
# appends compile and link flags.
set -euo pipefail
R=$(cd "$(dirname "$0")" && pwd)
FW=$(realpath "${FW_ROOT:-$R/firmware}")
MODE=${1:-native}; SINGLE=0
[ "${2:-}" = --single ] && SINGLE=1
case $MODE in native|san|wasm) ;; *) sed -n '7,15p' "$0" >&2; exit 2;; esac
[ $SINGLE = 1 ] && [ $MODE != wasm ] && { echo "--single goes with wasm" >&2; exit 2; }
[ -f "$FW/components/synth_core/synth_ui/synth_ui_task.c" ] ||
    { echo "no firmware at $FW (git submodule update --init --recursive)" >&2; exit 1; }
CC=gcc; DRUMS_AT=
OUT=$R/out/$MODE
if [ $MODE = wasm ]; then
    command -v emcc >/dev/null || { echo "wasm: no emcc on PATH (source emsdk_env.sh)" >&2; exit 1; }
    CC=emcc; DRUMS_AT=/components/amy/drums.bin
fi
H=$R/hostsim
HOST=$R/host
SC=$FW/components/synth_core
DISP=$FW/components/display
U8=$FW/components/u8g2/csrc
CFG=$R/config/sdkconfig.h
[ "${HOSTSIM_CLEAN:-0}" = 1 ] && rm -rf "$OUT/amy" "$OUT/obj" "$OUT/gen"
mkdir -p "$OUT/obj/amy" "$OUT/obj/sc" "$OUT/obj/ui" "$OUT/obj/u8g2" "$OUT/obj/host" "$OUT/gen"

# AMY as the firmware vendors it, at the device's 48 kHz: the host build
# takes amy.h's generic 44100 fallback, so the copy patches it.
if [ ! -d "$OUT/amy" ] || [ -n "$(find "$FW/components/amy/src" -newer "$OUT/amy/amy.h" -name '*.[ch]' | head -1)" ]; then
    rm -rf "$OUT/amy"; cp -r "$FW/components/amy/src" "$OUT/amy"
    sed -i 's/^#define AMY_SAMPLE_RATE 44100 *$/#define AMY_SAMPLE_RATE 48000/' "$OUT/amy/amy.h"
    grep -q "AMY_SAMPLE_RATE 44100" "$OUT/amy/amy.h" && { echo "48 kHz patch missed in amy.h" >&2; exit 1; }
fi

# The drum sample banks, from the samples of the AMY release the firmware
# pins, checked against the firmware's map of them.
DRUMS=$OUT/gen/drums.bin
MAP=$FW/components/amy/src/pcm_gamma9001.h
if [ ! -f "$DRUMS" ] || [ "$MAP" -nt "$DRUMS" ]; then
    python3 "$R/tools/gen-drums.py" "$FW" "$DRUMS" >/dev/null
fi
[ -n "$DRUMS_AT" ] || DRUMS_AT=$DRUMS

# stubs/ stands in for the ESP-IDF, FreeRTOS and driver headers the firmware
# includes (SHIMS.md).
CF=(-O2 -g -std=gnu11 -DAMY_WAVETABLE -DGAMMA9001 -DAMY_USE_FIXEDPOINT= -DHOSTSIM
    -DMALLOC_CAP_SPIRAM=0 -DMALLOC_CAP_8BIT=0 -DHOSTSIM_DRUMS="\"$DRUMS_AT\""
    -I"$R/stubs" -I"$HOST" -I"$R/config" -I"$OUT/amy" -I"$SC" -I"$SC/include"
    -I"$SC/sequencer_core" -I"$SC/project" -I"$SC/synth_ui" -I"$DISP"
    -I"$FW/components/seq_clamp" -I"$FW/components/project_store/include"
    -I"$FW/components/usb_audio/include" -I"$FW/components/wireless/include"
    -I"$FW/components/diagnostics/include" -I"$FW/components/my_buttons/include"
    -I"$FW/components/harness/include" -I"$FW/main" -I"$U8")
# Source paths in assert messages (__FILE__) ship in the wasm: keep them
# relative to the firmware or to this repo.
[ $MODE = wasm ] && CF+=(-g0 -ffile-prefix-map="$OUT/=" -ffile-prefix-map="$FW/="
                         -ffile-prefix-map="$R/=")
[ $MODE = san ] && CF+=(-fsanitize=address,undefined -fno-sanitize=shift
                        -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
# shellcheck disable=SC2206
[ -n "${HOSTSIM_CFLAGS:-}" ] && CF+=(${HOSTSIM_CFLAGS})

NJ=$(nproc)
FAILED="$OUT/obj/failed"
rm -f "$FAILED"
compile() {   # compile <obj> <src> <extra flags...>
    local o=$1 src=$2; shift 2
    if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ "$CFG" -nt "$o" ]; then
        while [ "$(jobs -rp | wc -l)" -ge "$NJ" ]; do wait -n || true; done
        { $CC "${CF[@]}" "$@" -c "$src" -o "$o" 2> "$o.err" || { rm -f "$o"; echo "$src" >> "$FAILED"; }; } &
    fi
}

AMY_MODS=(algorithms amy envelope examples parse filters oscillators pcm interp_partials custom
          delay log2_exp2 patches transfer sequencer libminiaudio-audio instrument amy_midi api
          midi_mappings cv_trigger)
for m in note_output; do [ -f "$OUT/amy/$m.c" ] && AMY_MODS+=("$m"); done
# Under Emscripten AMY would take its own web build (AudioWorklet hooks, a
# wasm-worker lock). Compiled without __EMSCRIPTEN__ it takes the same path
# as the native host build; its audio backend is left out (audio is off).
AMY_CF=()
if [ $MODE = wasm ]; then
    AMY_CF=(-U__EMSCRIPTEN__ -DAMY_NO_MINIAUDIO -DAMY_HOST_MIDI)
    AMY_MODS=("${AMY_MODS[@]/libminiaudio-audio}")
fi
for m in "${AMY_MODS[@]}"; do
    [ -n "$m" ] && compile "$OUT/obj/amy/$m.o" "$OUT/amy/$m.c" -w "${AMY_CF[@]}"
done

# The engine, plus what the UI reaches.
SC_SRCS=(amy_fx.c fx_bus.c arp_core.c quantizer.c prog_gen.c voice_config.c live_play.c
  sequencer_core/seq_chords.c sequencer_core/seq_core_dump.c sequencer_core/seq_core_editors.c
  sequencer_core/seq_core_engine.c sequencer_core/seq_core_progression.c
  sequencer_core/seq_core_snapshot.c sequencer_core/seq_core_state.c
  sequencer_core/seq_core_synth.c sequencer_core/seq_core_tempo.c sequencer_core/seq_core_trig.c
  sequencer_core/seq_trig_pump.c custompatches/drone_core.c custompatches/drone_std_core.c
  custompatches/bass_presets.c custompatches/clip_bounce.c custompatches/clip_player.c
  custompatches/drum_cache.c custompatches/wavetable_bank.c custompatches/sample_rec.c
  custompatches/wt_synth.c custompatches/wt_builder.c
  custompatches/fm_voice.c custompatches/fm_graph.c custompatches/fm_presets.c
  custompatches/additive_voice.c custompatches/additive_presets.c project/project_snapshot.c
  "$FW/components/project_store/project_store.c" "$FW/components/project_store/project_tlv.c"
  filter_scope.c project/project_templates.c)
# The template table firmware builds generate (synth_core CMakeLists.txt).
TPL_DIR=$SC/project/templates
TPL_C=$OUT/gen/project_templates_data.c
if [ ! -f "$TPL_C" ] || [ -n "$(find "$TPL_DIR" -newer "$TPL_C" -name '*.py' | head -1)" ]; then
    python3 "$TPL_DIR/gen_templates.py" --check-src "$FW/components" --out "$TPL_C" >/dev/null
fi
SC_SRCS+=("$TPL_C")
for f in "${SC_SRCS[@]}"; do
    src=$f; [[ $f = /* ]] || src=$SC/$f
    compile "$OUT/obj/sc/$(basename "${f%.c}").o" "$src" -w
done

# UI: every synth_ui source, the display renderers without the panel driver
# and its flush (hostsim keeps the buffer instead), the dispatcher, the
# harness interpreter.
for src in "$SC"/synth_ui/*.c; do compile "$OUT/obj/ui/$(basename "${src%.c}").o" "$src" -w; done
for src in "$DISP"/*.c; do
    case $(basename "$src") in priv_i2c_u8g2.c|display_flush.c|display_flush_runs.c) continue;; esac
    compile "$OUT/obj/ui/$(basename "${src%.c}").o" "$src" -w
done
compile "$OUT/obj/ui/input_dispatch.o" "$FW/main/input_dispatch.c" -w
compile "$OUT/obj/ui/usb_audio_watchdog.o" "$FW/components/usb_audio/usb_audio_watchdog.c" -w
compile "$OUT/obj/ui/harness_exec.o" "$FW/components/harness/harness_exec.c" -w -DCONFIG_DEV_SERIAL_HARNESS=1
for src in "$U8"/*.c; do compile "$OUT/obj/u8g2/$(basename "${src%.c}").o" "$src" -w; done

compile "$OUT/obj/host/host_glue.o" "$HOST/host_glue.c" -w
compile "$OUT/obj/host/host_pump.o" "$HOST/host_pump.c" -w
[ $MODE = wasm ] || compile "$OUT/obj/host/frame_out.o" "$HOST/frame_out.c" -Wall -Wextra
for src in "$H"/*.c; do
    compile "$OUT/obj/host/$(basename "${src%.c}").o" "$src" -Wall -Wextra -DCONFIG_DEV_SERIAL_HARNESS=1
done
wait
if [ -s "$FAILED" ]; then
    echo "compile failed:" >&2
    while read -r src; do echo "== $src" >&2; head -20 "$OUT/obj/"*/"$(basename "${src%.c}").o.err" >&2; done < "$FAILED"
    exit 1
fi

OBJS=("$OUT"/obj/host/*.o "$OUT"/obj/ui/*.o "$OUT"/obj/sc/*.o "$OUT"/obj/u8g2/*.o "$OUT"/obj/amy/*.o)
if [ $MODE != wasm ]; then
    gcc "${CF[@]}" "${OBJS[@]}" -lz -lm -pthread -o "$OUT/hostsim"
    echo "built $OUT/hostsim"
    exit 0
fi

# The static site: the module, its preloaded data, the page. Links run in
# their output directory so the loader names its data file without a path.
SITE=$OUT/site
VERSION=${HOSTSIM_VERSION:-$(git -C "$FW" describe --tags --always --dirty)}
[[ $VERSION =~ ^[A-Za-z0-9._+-]{1,64}$ ]] || { echo "HOSTSIM_VERSION: 1-64 of A-Z a-z 0-9 . _ + -" >&2; exit 1; }
rm -rf "$SITE"
mkdir -p "$SITE" "$OUT/node" "$OUT/node-check"
PRELOAD=(--preload-file "$DRUMS@$DRUMS_AT")
STACK=${HOSTSIM_WASM_STACK:-1MB}
LF=(-lm -sMODULARIZE=1 -sEXPORT_NAME=createHostsim -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=64MB -sSTACK_SIZE="$STACK" --no-entry
    -sINCOMING_MODULE_JS_API=wasmBinary,getPreloadedPackage,locateFile,print,printErr
    -sEXPORTED_FUNCTIONS=_hs_boot,_hs_step,_hs_json,_hs_json_len,_hs_frame,_hs_frame_len,_hs_audio,_hs_audio_len,_malloc,_free)
RT=FS,HEAPU8,HEAP16,UTF8ToString,stringToNewUTF8
(cd "$SITE" && emcc "${CF[@]}" "${OBJS[@]}" "${LF[@]}" "${PRELOAD[@]}" -lidbfs.js -sEXPORTED_RUNTIME_METHODS=$RT,IDBFS \
    -sENVIRONMENT=web -o hostsim.js)
# The same module for node (hostsim/replay-wasm.mjs), and a checking build:
# wasm assertions, every load and store bounds-checked, stack overflow
# detection.
(cd "$OUT/node" && emcc "${CF[@]}" "${OBJS[@]}" "${LF[@]}" "${PRELOAD[@]}" -sEXPORTED_RUNTIME_METHODS=$RT \
    -sENVIRONMENT=node -o hostsim.js)
(cd "$OUT/node-check" && emcc "${CF[@]}" "${OBJS[@]}" "${LF[@]}" "${PRELOAD[@]}" -sEXPORTED_RUNTIME_METHODS=$RT \
    -sENVIRONMENT=node -sASSERTIONS=2 -sSAFE_HEAP=1 -sSTACK_OVERFLOW_CHECK=2 --profiling-funcs -o hostsim.js)

# The page with the in-browser backend: relative URLs, the version, a title
# instead of the lab's navigation.
sed -e 's|href="/common.css"|href="common.css"|' -e 's|src="/common.js"|src="common.js"|' \
    -e 's|<script src="common.js"></script>|<script src="common.js"></script>\n<script src="hostsim.js"></script>\n<script src="wasm.js"></script>|' \
    -e "s|<meta charset=\"utf-8\">|<meta charset=\"utf-8\">\n<meta name=\"firmware-version\" content=\"$VERSION\">|" \
    -e 's|<title>lab: device</title>|<title>S3-Amysynth in the browser</title>\n<link rel="icon" href="data:,">|' \
    -e 's|<nav><a href="/">lab</a> / device</nav>|<nav>S3-Amysynth: the device in the browser</nav>|' \
    "$R/page/index.html" > "$SITE/index.html"
for want in 'src="wasm.js"' 'name="firmware-version"' '<nav>S3-Amysynth'; do
    grep -q "$want" "$SITE/index.html" || { echo "page patch missed: $want" >&2; exit 1; }
done
cp "$R/page/wasm.js" "$R/page/common.css" "$R/page/common.js" "$SITE/"
cp "$FW/CONTROLS.md" "$SITE/controls.md"
# GitHub Pages runs Jekyll unless told not to, and Jekyll renders controls.md
# to controls.html, which the page does not fetch.
touch "$SITE/.nojekyll"

# Notices: what this build contains, then the firmware's attributions and the
# licence texts of everything compiled in.
{
    echo "S3-Amysynth in the browser, firmware $VERSION"
    echo
    echo "This page runs the firmware's application code and the AMY synthesis engine"
    echo "compiled to WebAssembly with Emscripten. Compiled in: the project's own code"
    echo "(MIT), AMY with its patch and sample data (MIT), U8g2 and its X11 misc-fixed"
    echo "fonts (BSD-2-Clause; fonts public domain), the Emscripten runtime and musl libc."
    echo "hostsim.data holds AMY's Gamma9001 drum sample banks, distributed as part of AMY."
    echo "AMY describes the set as the Koblo Tokyo drum machines, an 808 SoundFont gap-fill"
    echo "and the AG-10 Power Kit."
    echo "Not compiled in: ESP-IDF, TinyUSB, usb_device_uac, LittleFS, NimBLE, miniaudio."
    for f in ATTRIBUTIONS.md LICENSE components/amy/LICENSE components/u8g2/LICENSE; do
        printf '\n\n==== %s ====\n\n' "$f"; cat "$FW/$f"
    done
    EM=$(dirname "$(command -v emcc)")
    for f in "$EM/LICENSE" "$EM/system/lib/libc/musl/COPYRIGHT"; do
        [ -f "$f" ] && { printf '\n\n==== %s ====\n\n' "Emscripten: ${f#"$EM"/}"; cat "$f"; }
    done
} > "$SITE/notices.txt"
echo "built $SITE (firmware $VERSION; serve it, open index.html)"

# --single: the module carries the wasm and drums.bin inside its JS; the
# page carries its scripts, CONTROLS.md and the notices.
if [ $SINGLE = 1 ]; then
    mkdir -p "$OUT/single"
    (cd "$OUT/single" && emcc "${CF[@]}" "${OBJS[@]}" "${LF[@]}" --embed-file "$DRUMS@$DRUMS_AT" -lidbfs.js \
        -sEXPORTED_RUNTIME_METHODS=$RT,IDBFS -sENVIRONMENT=web -sSINGLE_FILE=1 \
        -sSINGLE_FILE_BINARY_ENCODE=0 -Wno-unused-command-line-argument -o hostsim.js)
    python3 "$H/single_file.py" "$SITE" "$OUT/single/hostsim.js" "$OUT/S3-Amysynth-$VERSION.html"
fi
