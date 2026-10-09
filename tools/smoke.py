#!/usr/bin/env python3
"""Smoke replay: does a build boot, play and take input?

    tools/smoke.py [--wasm] [workdir]

Asks the native build (out/native/hostsim) for the built-in templates
(`pr.tpl`), then replays a log that loads each one the way the page's
picker does (load, play 200 ms later) and lets it play for about 2.7 s, and
finally opens the menu with button 3. Checks, per build:

  - the replay exits 0
  - every template plays above FLOOR_DB (RMS over its last 2.7 s)
  - the frame changes when button 3 is pressed

--wasm runs the same log through the node build of the module as well
(out/wasm/node/hostsim.js, hostsim/replay-wasm.mjs). Exits 1 on the first
failed check, after printing what failed. Standard library only.
"""
import array, math, os, subprocess, sys, tempfile, wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIVE = os.path.join(ROOT, 'out/native/hostsim')
NODE_MOD = os.path.join(ROOT, 'out/wasm/node/hostsim.js')
REPLAY_WASM = os.path.join(ROOT, 'hostsim/replay-wasm.mjs')

BLOCK = 256           # frames per block
PER_TPL = 600         # blocks per template, 3.2 s
PLAY_AT = 40          # play 213 ms after the load, as the page's 200 ms
SKIP = 100            # level measured from here to the next template
FLOOR_DB = -60.0      # the templates measure -36 to -29 dBFS


def fail(msg):
    print(f'smoke: FAIL: {msg}')
    sys.exit(1)


def replay(kind, log, out):
    if kind == 'native':
        cmd = [NATIVE, 'replay', log, out]
    else:
        cmd = ['node', REPLAY_WASM, NODE_MOD, log, out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout[-2000:] + r.stderr[-2000:])
        fail(f'{kind} replay exited {r.returncode}')


def template_count(work):
    log = os.path.join(work, 'list.log')
    with open(log, 'w') as f:
        f.write('@0 pr.tpl\n@1 end\n')
    out = os.path.join(work, 'list')
    replay('native', log, out)
    with open(os.path.join(out, 'console.txt')) as f:
        names = [l.split(' ', 2)[2].strip() for l in f if l.startswith('TPL ')]
    if not names:
        fail('pr.tpl lists no templates')
    return names


def write_log(path, n):
    end = n * PER_TPL
    with open(path, 'w') as f:
        for i in range(n):
            f.write(f'@{i * PER_TPL} pr.tpl {i}\n@{i * PER_TPL + PLAY_AT} tr.play\n')
        f.write(f'@{end} shot before\n@{end + 1} key 3 down\n@{end + 5} key 3 up\n'
                f'@{end + 100} shot after\n@{end + 150} end\n')


def level_db(samples, ch, first, last):
    x = samples[first * BLOCK * ch:last * BLOCK * ch]
    rms = math.sqrt(sum(v * v for v in x) / max(len(x), 1))
    return 20 * math.log10(rms / 32768) if rms > 0 else -math.inf


def check(kind, out, names):
    with wave.open(os.path.join(out, 'out.wav')) as w:
        ch = w.getnchannels()
        samples = array.array('h', w.readframes(w.getnframes()))
    worst = None
    for i, name in enumerate(names):
        db = level_db(samples, ch, i * PER_TPL + SKIP, (i + 1) * PER_TPL)
        if db < FLOOR_DB:
            fail(f'{kind}: template {i} {name} plays at {db:.1f} dBFS (floor {FLOOR_DB})')
        worst = db if worst is None else min(worst, db)
    with open(os.path.join(out, 'before.txt')) as a, open(os.path.join(out, 'after.txt')) as b:
        if a.read().split('\n', 1)[1] == b.read().split('\n', 1)[1]:
            fail(f'{kind}: frame unchanged after button 3')
    print(f'smoke: {kind} ok: {len(names)} templates play (quietest {worst:.1f} dBFS), button 3 opens the menu')


def main():
    args = sys.argv[1:]
    wasm = '--wasm' in args
    args = [a for a in args if a != '--wasm']
    work = args[0] if args else tempfile.mkdtemp(prefix='smoke-')
    os.makedirs(work, exist_ok=True)
    names = template_count(work)
    log = os.path.join(work, 'smoke.log')
    write_log(log, len(names))
    for kind in ['native'] + (['wasm'] if wasm else []):
        out = os.path.join(work, kind)
        replay(kind, log, out)
        check(kind, out, names)


if __name__ == '__main__':
    main()
