#!/usr/bin/env python3
"""Fold the static site into one HTML file that opens from disk (file://).

    single_file.py <site dir> <single-file hostsim.js> <out.html>

<single-file hostsim.js> is the module linked with -sSINGLE_FILE and
--embed-file, so the wasm and drums.bin are inside it. One file: the device
page, its stylesheet and scripts inlined, the module (id hostsim-module) in
place of hostsim.js, and CONTROLS.md and the notices as hidden textareas it
reads instead of fetching. The two lab pages ride along as base64 documents
(lab-wt-b64, lab-fm-b64) with a placeholder where the module goes; the Tools
links open them in new tabs written from those documents and the page's own
module text (wasm.js), and their Apply reaches the device page over
postMessage to window.opener (lab.js).
"""
import base64
import html
import re
import sys
from pathlib import Path

site, module_js, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
module = module_js.read_text()


def script(text, attrs=''):
    # Nothing inside a <script> may close it.
    return '<script%s>\n' % attrs + re.sub(r'</(script)', r'<\\/\1', text, flags=re.I) + '\n</script>'


def swap(page, old, new):
    if old not in page:
        sys.exit('single_file.py: page has no %r' % old)
    return page.replace(old, new, 1)


# The lab pages, one directory down in the site, as documents the device page
# writes into a new tab: the module goes in at <!--HOSTSIM-MODULE-->, and the
# back link focuses the device tab.
labs = {}
for name in ('wt', 'fm'):
    lab = (site / name / 'index.html').read_text()
    lab = swap(lab, '<link rel="stylesheet" href="../common.css">', '<style>\n%s\n</style>' % (site / 'common.css').read_text())
    lab = swap(lab, '<script src="../common.js"></script>', script((site / 'common.js').read_text()))
    lab = swap(lab, '<script src="../hostsim.js"></script>', '<!--HOSTSIM-MODULE-->')
    lab = swap(lab, '<script src="../lab.js"></script>', script((site / 'lab.js').read_text()))
    lab = swap(lab, '<meta charset="utf-8">', '<meta charset="utf-8">\n<meta name="single-file" content="1">')
    lab = swap(lab, 'href="../"', 'href="#" data-device-focus')
    labs[name] = base64.b64encode(lab.encode('utf-8')).decode('ascii')

# The device page.
page = (site / 'index.html').read_text()
page = swap(page, '<link rel="stylesheet" href="common.css">', '<style>\n%s\n</style>' % (site / 'common.css').read_text())
page = swap(page, '<script src="common.js"></script>', script((site / 'common.js').read_text()))
page = swap(page, '<script src="hostsim.js"></script>', script(module, ' id="hostsim-module"'))
page = swap(page, '<script src="wasm.js"></script>', script((site / 'wasm.js').read_text()))
page = swap(page, '<meta name="firmware-version"', '<meta name="single-file" content="1">\n<meta name="firmware-version"')
for name in labs:
    page = swap(page, 'href="%s/" target="_blank"' % name, 'href="#" data-lab="%s"' % name)
# Before the scripts, which read them while loading.
page = swap(page, '<footer id="notices"', '<textarea id="controls-md" hidden>%s</textarea>\n<textarea id="notices-txt" hidden>%s</textarea>\n%s\n<footer id="notices"'
            % (html.escape((site / 'controls.md').read_text()), html.escape((site / 'notices.txt').read_text()),
               '\n'.join('<script type="text/plain" id="lab-%s-b64">%s</script>' % (name, b64) for name, b64 in labs.items())))
out.write_text(page)
print('%s: %d bytes' % (out, out.stat().st_size))
