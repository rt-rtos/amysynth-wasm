#!/usr/bin/env bash
# Refresh config/sdkconfig.h, the host build's Kconfig, from a firmware build:
# the device's generated header with CONFIG_SYNTH_WIRELESS cleared (no BLE on
# the host). The firmware repo does not track its sdkconfig, so the snapshot
# is committed here; refresh it when the firmware's configuration changes.
#
# Usage: tools/update-config.sh <firmware build dir>   (e.g. ../S3-Amysynth/build)
set -euo pipefail
[ $# -eq 1 ] || { sed -n '7p' "$0" >&2; exit 2; }
SRC=$1/config/sdkconfig.h
[ -f "$SRC" ] || { echo "no $SRC: build the firmware first" >&2; exit 1; }
OUT=$(dirname "$0")/../config/sdkconfig.h
grep -v '^#define CONFIG_SYNTH_WIRELESS ' "$SRC" > "$OUT"
echo "wrote $OUT"
