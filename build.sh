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
# The build is the CMake project in CMakeLists.txt, configured into
# out/<mode>/build and built with Ninja, which rebuilds what changed;
# HOSTSIM_CLEAN=1 starts over. HOSTSIM_CFLAGS appends compile and link flags,
# HOSTSIM_WASM_STACK sets the wasm shadow stack (default 1MB).
set -euo pipefail
R=$(cd "$(dirname "$0")" && pwd)
FW=$(realpath "${FW_ROOT:-$R/firmware}")
MODE=${1:-native}; SINGLE=0
[ "${2:-}" = --single ] && SINGLE=1
case $MODE in native|san|wasm) ;; *) sed -n '7,15p' "$0" >&2; exit 2;; esac
[ $SINGLE = 1 ] && [ $MODE != wasm ] && { echo "--single goes with wasm" >&2; exit 2; }
[ -f "$FW/components/synth_core/synth_ui/synth_ui_task.c" ] ||
    { echo "no firmware at $FW (git submodule update --init --recursive)" >&2; exit 1; }
if [ $MODE = wasm ]; then
    command -v emcc >/dev/null || { echo "wasm: no emcc on PATH (source emsdk_env.sh)" >&2; exit 1; }
fi
OUT=$R/out/$MODE
B=$OUT/build
# The build directory, and what the build script before the CMake project
# generated beside it.
[ "${HOSTSIM_CLEAN:-0}" = 1 ] && rm -rf "$B" "$OUT/amy" "$OUT/obj" "$OUT/gen" "$OUT/single"

# Configure when there is no build directory yet, or when a setting it was
# configured with changed.
EXTRA=${HOSTSIM_CFLAGS:-}
STACK=${HOSTSIM_WASM_STACK:-1MB}
cached() { sed -n "s/^$1:[A-Z]*=//p" "$B/CMakeCache.txt"; }
if [ ! -f "$B/build.ninja" ] || [ "$(cached FW_ROOT)" != "$FW" ] ||
   [ "$(cached HOSTSIM_EXTRA_FLAGS)" != "$EXTRA" ] || [ "$(cached HOSTSIM_WASM_STACK)" != "$STACK" ]; then
    CONF=(cmake -S "$R" -B "$B" -G Ninja -DHOSTSIM_MODE="$MODE" -DFW_ROOT="$FW"
          -DHOSTSIM_EXTRA_FLAGS="$EXTRA" -DHOSTSIM_WASM_STACK="$STACK")
    if [ $MODE = wasm ]; then emcmake "${CONF[@]}"; else "${CONF[@]}" -DCMAKE_C_COMPILER=gcc; fi
fi

TARGETS=(all)
[ $SINGLE = 1 ] && TARGETS+=(single)
cmake --build "$B" -j "$(nproc)" --target "${TARGETS[@]}"

if [ $MODE != wasm ]; then
    echo "built $OUT/hostsim"
    exit 0
fi
VERSION=$(sed -n 's/.*<meta name="firmware-version" content="\([^"]*\)">.*/\1/p' "$OUT/site/index.html")
echo "built $OUT/site (firmware $VERSION; serve it, open index.html)"
