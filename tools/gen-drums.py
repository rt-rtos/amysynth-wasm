#!/usr/bin/env python3
"""Generate the gamma9001 drums.bin blob the firmware reads from its 'drums'
flash partition, and hostsim from its preloaded data file.

Standalone re-implementation of the bin-writing half of upstream
amy.headers.generate_gamma9001_headers(): concatenates every non-tr808 bank
sample from the firmware's amy submodule, sounds/gamma9001/ (manifest order,
mono int16 LE @22050 Hz). Does not regenerate any header: the vendored
components/amy/src/pcm_gamma9001.h is the map this blob must match, and the
script verifies every offset/length pair against it before writing.

Usage: python3 tools/gen-drums.py <firmware root> <out file>
"""
import json
import os
import re
import sys
import wave

if len(sys.argv) != 3:
    sys.exit(__doc__.strip().splitlines()[-1])
FW = sys.argv[1]
SOUNDS = os.path.join(FW, "amy", "sounds", "gamma9001")
VENDORED_MAP = os.path.join(FW, "components", "amy", "src", "pcm_gamma9001.h")
OUT = sys.argv[2]
ROM_BANK = "tr808"  # baked into pcm_gamma808.h, not part of drums.bin
SAMPLE_RATE = 22050


def read_wav_mono16(path):
    w = wave.open(path)
    assert w.getnchannels() == 1 and w.getsampwidth() == 2, path
    assert w.getframerate() == SAMPLE_RATE, f"{path}: {w.getframerate()}"
    data = w.readframes(w.getnframes())
    w.close()
    return data  # WAV payload is already little-endian int16


def parse_vendored_map():
    """Return (num_samples, bin_frames, [(offset, frames), ...])."""
    text = open(VENDORED_MAP).read()
    num = int(re.search(r"#define GAMMA9001_NUM_SAMPLES (\d+)", text).group(1))
    frames = int(re.search(r"#define GAMMA9001_BIN_FRAMES (\d+)", text).group(1))
    entries = re.findall(r"/\* \[\d+\] preset \d+ \*/ \{(\d+), (\d+),", text)
    return num, frames, [(int(a), int(b)) for a, b in entries]


def main():
    manifest = json.load(open(os.path.join(SOUNDS, "manifest.json")))
    entries = [m for m in manifest if m["bank"] != ROM_BANK]
    num, total_frames, vmap = parse_vendored_map()

    if len(entries) != num or len(vmap) != num:
        sys.exit(f"count mismatch: manifest {len(entries)} vs vendored map {num}")

    blob = bytearray()
    for i, m in enumerate(entries):
        data = read_wav_mono16(os.path.join(SOUNDS, m["file"]))
        off, frames = len(blob) // 2, len(data) // 2
        if (off, frames) != vmap[i]:
            sys.exit(f"[{i}] {m['file']}: ({off}, {frames}) != vendored {vmap[i]}")
        blob += data

    if len(blob) != total_frames * 2:
        sys.exit(f"total {len(blob)} bytes != GAMMA9001_BIN_FRAMES*2 {total_frames * 2}")

    with open(OUT, "wb") as f:
        f.write(blob)
    print(f"OK: {OUT} ({len(blob)} bytes, {num} samples, map verified)")


if __name__ == "__main__":
    main()
