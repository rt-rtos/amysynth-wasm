#!/usr/bin/env python3
"""Fold the static site into one HTML file that opens from disk (file://).

    single_file.py <site dir> <single-file hostsim.js> <out.html>

<single-file hostsim.js> is the module linked with -sSINGLE_FILE and
--embed-file, so the wasm and drums.bin are inside it. The page's stylesheet
and scripts are inlined, the module replaces the site's hostsim.js, and
CONTROLS.md and the notices travel as hidden textareas the page reads
instead of fetching.
"""
import html
import re
import sys
from pathlib import Path

site, module_js, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
page = (site / 'index.html').read_text()


def script(text):
    # Nothing inside a <script> may close it.
    return '<script>\n' + re.sub(r'</(script)', r'<\\/\1', text, flags=re.I) + '\n</script>'


def swap(old, new):
    global page
    if old not in page:
        sys.exit('single_file.py: page has no %r' % old)
    page = page.replace(old, new, 1)


swap('<link rel="stylesheet" href="common.css">', '<style>\n%s\n</style>' % (site / 'common.css').read_text())
swap('<script src="common.js"></script>', script((site / 'common.js').read_text()))
swap('<script src="hostsim.js"></script>', script(module_js.read_text()))
swap('<script src="wasm.js"></script>', script((site / 'wasm.js').read_text()))
swap('<meta name="firmware-version"', '<meta name="single-file" content="1">\n<meta name="firmware-version"')
# Before the scripts, which read them while loading.
swap('<footer id="notices"', '<textarea id="controls-md" hidden>%s</textarea>\n<textarea id="notices-txt" hidden>%s</textarea>\n<footer id="notices"'
     % (html.escape((site / 'controls.md').read_text()), html.escape((site / 'notices.txt').read_text())))
out.write_text(page)
print('%s: %d bytes' % (out, out.stat().st_size))
