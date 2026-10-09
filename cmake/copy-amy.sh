#!/bin/sh
# copy-amy.sh <amy src> <copy>: AMY as the firmware vendors it, at the
# device's 48 kHz. The host build takes amy.h's generic 44100 fallback, so
# the copy patches it.
set -e
rm -rf "$2"
cp -r "$1" "$2"
sed -i 's/^#define AMY_SAMPLE_RATE 44100 *$/#define AMY_SAMPLE_RATE 48000/' "$2/amy.h"
if grep -q "AMY_SAMPLE_RATE 44100" "$2/amy.h"; then
    echo "48 kHz patch missed in amy.h" >&2
    rm -rf "$2"
    exit 1
fi
