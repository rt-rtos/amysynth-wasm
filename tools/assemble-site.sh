#!/usr/bin/env bash
# The static site around the linked module, and the single-file page. Run by
# the wasm build (CMakeLists.txt) on every build, so the version is current.
#
#   assemble-site.sh site <repo> <firmware> <out dir> <emscripten dir>
#       the page, its scripts, controls.md, .nojekyll and notices.txt into
#       <out dir>/site, beside the module the build linked there
#   assemble-site.sh single <repo> <firmware> <out dir> <single-file hostsim.js>
#       <out dir>/S3-Amysynth-<version>.html from the site and the module
#       linked with -sSINGLE_FILE
#
# The version is `git describe` of the firmware tree, or HOSTSIM_VERSION.
set -euo pipefail
[ $# = 5 ] || { sed -n '5,10p' "$0" >&2; exit 2; }
STEP=$1; R=$2; FW=$3; OUT=$4
SITE=$OUT/site
VERSION=${HOSTSIM_VERSION:-$(git -C "$FW" describe --tags --always --dirty)}
[[ $VERSION =~ ^[A-Za-z0-9._+-]{1,64}$ ]] || { echo "HOSTSIM_VERSION: 1-64 of A-Z a-z 0-9 . _ + -" >&2; exit 1; }

case $STEP in
single)
    python3 "$R/hostsim/single_file.py" "$SITE" "$5" "$OUT/S3-Amysynth-$VERSION.html"
    exit 0;;
site) EM=$5;;
*) sed -n '5,10p' "$0" >&2; exit 2;;
esac

# The site holds only what this build puts there: the module the build
# linked, and what follows.
find "$SITE" -mindepth 1 -maxdepth 1 ! -name hostsim.js ! -name hostsim.wasm ! -name hostsim.data \
    -exec rm -rf {} +

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
    for f in "$EM/LICENSE" "$EM/system/lib/libc/musl/COPYRIGHT"; do
        [ -f "$f" ] && { printf '\n\n==== %s ====\n\n' "Emscripten: ${f#"$EM"/}"; cat "$f"; }
    done
} > "$SITE/notices.txt"
