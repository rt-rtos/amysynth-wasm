#!/usr/bin/env python3
"""Does the web configuration know every symbol the firmware declares?

    tools/kconfig-gate.py [firmware dir] [config header]

config/sdkconfig.h is the web build's own configuration, kept by hand and
independent of the device's: a symbol is either defined there or listed as
`/* CONFIG_X is not set */`. A firmware update that adds a Kconfig symbol
needs a decision for the web, so this lists every symbol declared in the
firmware's Kconfig files (default: the `firmware` submodule) that the header
neither defines nor lists as not set, with the Kconfig lines that describe
it, and exits 1 when there is one. Changed defaults, ranges and prompts, and
removed symbols, do not need anything here.
"""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DECL = re.compile(r'\s*(?:menu)?config\s+(\w+)')
ATTR = re.compile(r'(bool|int|hex|string|tristate|default|depends on|range)\b')


def kconfig_files(fw):
    for f in sorted(glob.glob(os.path.join(fw, '**', 'Kconfig*'), recursive=True)):
        rel = os.path.relpath(f, fw)
        # amy/ is the pristine upstream submodule; the build uses components/amy
        if not rel.startswith(('amy/', 'managed_components/', 'build/')):
            yield rel, f


def declared(fw):
    """CONFIG_NAME -> (file, the type/default/depends/range lines)."""
    syms = {}
    for rel, path in kconfig_files(fw):
        lines = open(path).read().splitlines()
        for i, line in enumerate(lines):
            m = DECL.match(line)
            if not m:
                continue
            attrs = []
            for nxt in lines[i + 1:]:
                t = nxt.strip()
                if not t:
                    continue
                if not ATTR.match(t):
                    break
                attrs.append(' '.join(t.split()))
            syms.setdefault('CONFIG_' + m.group(1), (rel, attrs))
    return syms


def main():
    args = sys.argv[1:]
    fw = args[0] if args else os.path.join(ROOT, 'firmware')
    header = args[1] if len(args) > 1 else os.path.join(ROOT, 'config/sdkconfig.h')
    text = open(header).read()
    defined = set(re.findall(r'^#define\s+(CONFIG_\w+)', text, re.M))
    not_set = set(re.findall(r'^/\*\s*(CONFIG_\w+) is not set\s*\*/', text, re.M))
    both = sorted(defined & not_set)
    if both:
        print(f'{header}: defined and listed as not set: {" ".join(both)}')
        sys.exit(1)
    syms = declared(fw)
    missing = sorted(s for s in syms if s not in defined and s not in not_set)
    for s in sorted(not_set - set(syms)):
        print(f'note: {s} is listed as not set but the firmware no longer declares it')
    if not missing:
        print(f'Kconfig: all {len(syms)} firmware symbols have a web setting')
        return
    shown = os.path.relpath(header, ROOT)
    print(f'Firmware Kconfig symbols with no web setting in {header if shown.startswith("..") else shown}:')
    for s in missing:
        rel, attrs = syms[s]
        print(f'  {s}  ({rel}: {"; ".join(attrs) or "no type line"})')
    print('Give each one a value (#define) or a "/* CONFIG_X is not set */" line, then push.')
    sys.exit(1)


if __name__ == '__main__':
    main()
