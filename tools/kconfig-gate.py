#!/usr/bin/env python3
"""Does a firmware update need a new config/sdkconfig.h?

    tools/kconfig-gate.py <firmware repo> <old commit> <new commit>

config/sdkconfig.h is a snapshot of the device's generated header
(tools/update-config.sh), taken from a firmware build; the host cannot make
one. A firmware update that changes its Kconfig files (a symbol added,
removed or renamed, a type, default, range, dependency or choice changed)
may need a new snapshot. Compares every Kconfig file of the two commits with
help texts, prompt texts, comments and blank lines dropped, prints what
changed, and exits 1 when anything did. Help and prompt edits alone pass.
"""
import difflib, re, subprocess, sys


def git(repo, *args):
    return subprocess.run(['git', '-C', repo, *args], check=True,
                          capture_output=True, text=True).stdout


def kconfig_files(repo, commit):
    names = git(repo, 'ls-tree', '-r', '--name-only', commit).splitlines()
    return sorted(n for n in names if re.search(r'(^|/)Kconfig[^/]*$', n))


def significant(text):
    """The lines that define something: no help text, comments or blanks."""
    out, help_indent = [], None
    for line in text.expandtabs(8).splitlines():
        stripped = line.strip()
        indent = len(line) - len(line.lstrip())
        if help_indent is not None:
            if not stripped or indent > help_indent:
                continue
            help_indent = None
        if stripped in ('help', '---help---'):
            help_indent = indent
            continue
        if not stripped or stripped.startswith('#'):
            continue
        if not stripped.startswith('default'):
            stripped = re.sub(r'"(\\.|[^"\\])*"', '""', stripped)   # prompt text
        out.append(' '.join(stripped.split()))
    return out


def snapshot(repo, commit):
    return {f: significant(git(repo, 'show', f'{commit}:{f}'))
            for f in kconfig_files(repo, commit)}


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.split('\n\n')[1])
    repo, old, new = sys.argv[1:]
    a, b = snapshot(repo, old), snapshot(repo, new)
    changed = False
    for f in sorted(set(a) | set(b)):
        if a.get(f) == b.get(f):
            continue
        changed = True
        if f not in a or f not in b:
            print(f'{f}: {"added" if f in b else "removed"}')
            continue
        print(f'{f}:')
        for l in list(difflib.unified_diff(a[f], b[f], lineterm='', n=1))[2:]:
            print(f'  {l}')
    if changed:
        print('Kconfig changed: refresh config/sdkconfig.h with tools/update-config.sh '
              'from a firmware build of the new commit, and bump the submodule by hand.')
        sys.exit(1)
    print('Kconfig: no change outside help and prompt texts')


if __name__ == '__main__':
    main()
