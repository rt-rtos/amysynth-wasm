#!/usr/bin/env python3
"""Fold the static site into HTML files that open from disk (file://).

    single_file.py <site dir> <single-file hostsim.js> <out.html>

<single-file hostsim.js> is the module linked with -sSINGLE_FILE and
--embed-file, so the wasm and drums.bin are inside it. Three files: the
device page at <out.html>, and the two lab pages beside it as
<out>-wt.html and <out>-fm.html, each with its own copy of the module. A
page's stylesheet and scripts are inlined, the module replaces hostsim.js,
and the device page carries CONTROLS.md and the notices as hidden textareas
it reads instead of fetching. The pages link each other by file name.
Apply between the lab and device pages needs a BroadcastChannel across
file:// pages, which browsers may refuse; the served site has no such limit.
"""
import html
import re
import sys
from pathlib import Path

site, module_js, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
module = module_js.read_text()
lab_out = {name: out.with_name('%s-%s.html' % (out.stem, name)) for name in ('wt', 'fm')}


def script(text):
    # Nothing inside a <script> may close it.
    return '<script>\n' + re.sub(r'</(script)', r'<\\/\1', text, flags=re.I) + '\n</script>'


def swap(page, old, new):
    if old not in page:
        sys.exit('single_file.py: page has no %r' % old)
    return page.replace(old, new, 1)


# The device page.
page = (site / 'index.html').read_text()
page = swap(page, '<link rel="stylesheet" href="common.css">', '<style>\n%s\n</style>' % (site / 'common.css').read_text())
page = swap(page, '<script src="common.js"></script>', script((site / 'common.js').read_text()))
page = swap(page, '<script src="hostsim.js"></script>', script(module))
page = swap(page, '<script src="wasm.js"></script>', script((site / 'wasm.js').read_text()))
page = swap(page, '<meta name="firmware-version"', '<meta name="single-file" content="1">\n<meta name="firmware-version"')
for name, path in lab_out.items():
    page = swap(page, 'href="%s/"' % name, 'href="%s"' % path.name)
# Before the scripts, which read them while loading.
page = swap(page, '<footer id="notices"', '<textarea id="controls-md" hidden>%s</textarea>\n<textarea id="notices-txt" hidden>%s</textarea>\n<footer id="notices"'
            % (html.escape((site / 'controls.md').read_text()), html.escape((site / 'notices.txt').read_text())))
out.write_text(page)
print('%s: %d bytes' % (out, out.stat().st_size))

# The lab pages, one directory down in the site.
for name, path in lab_out.items():
    page = (site / name / 'index.html').read_text()
    page = swap(page, '<link rel="stylesheet" href="../common.css">', '<style>\n%s\n</style>' % (site / 'common.css').read_text())
    page = swap(page, '<script src="../common.js"></script>', script((site / 'common.js').read_text()))
    page = swap(page, '<script src="../hostsim.js"></script>', script(module))
    page = swap(page, '<script src="../lab.js"></script>', script((site / 'lab.js').read_text()))
    page = swap(page, '<meta charset="utf-8">', '<meta charset="utf-8">\n<meta name="single-file" content="1">')
    page = swap(page, 'href="../"', 'href="%s"' % out.name)
    path.write_text(page)
    print('%s: %d bytes' % (path, path.stat().st_size))
