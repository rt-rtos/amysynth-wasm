#!/usr/bin/env node
// The lab calls of hostsim's WebAssembly module (hostsim/lab.c) under node:
// boots an instance the way page/lab.js does and checks that each call
// answers and that both labs sound. Prints the boot time and the time one
// 1-block step takes; exits 1 on a failed check.
//
//   node lab-check.mjs [module.js]      (default out/wasm/node/hostsim.js)
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const modPath = path.resolve(process.argv[2] || path.join(here, '..', 'out', 'wasm', 'node', 'hostsim.js'));
const require = createRequire(import.meta.url);
const createHostsim = require(modPath);

let failed = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failed++; };

const t0 = performance.now();
const modDir = path.dirname(modPath);
const M = await createHostsim({
  locateFile: f => path.join(modDir, f),
  print: () => {},
  printErr: t => process.stderr.write(t + '\n'),
});
M._hs_boot();
const step = (blocks, input = []) => {
  const p = M.stringToNewUTF8(input.join('\n'));
  const r = M._hs_step(blocks, p);
  M._free(p);
  if (r !== 0) throw new Error('hs_step failed');
  const st = JSON.parse(M.UTF8ToString(M._hs_json(), M._hs_json_len()));
  return { st, audio: new Int16Array(M.HEAPU8.slice(M._hs_audio(), M._hs_audio() + M._hs_audio_len()).buffer) };
};
const boot = step(1, ['cmd 0 tr.stop', 'cmd 0 amy.wire V5']);
console.log(`boot: ${(performance.now() - t0).toFixed(0)} ms (module instantiate + hs_boot + one step)`);
console.log(`boot step console: ${JSON.stringify(boot.st.console.trim())}`);

const rms = a => { let s = 0; for (const x of a) s += x * x; return Math.sqrt(s / Math.max(1, a.length)); };
const steps = (n, input) => { const parts = []; for (let b = 0; b < n; b++) parts.push(step(1, b ? [] : input).audio); return parts; };
const cat = parts => { const out = new Int16Array(parts.reduce((n, p) => n + p.length, 0)); let o = 0; for (const p of parts) { out.set(p, o); o += p.length; } return out; };

// fm: the default voice.
const fv = JSON.parse(M.UTF8ToString(M._lab_fm_voice(0, 0, M.stringToNewUTF8('describe'), 0, 0, 0)));
check(Array.isArray(fv.ops) && fv.ops.length === 6 && fv.v.length === 83, `lab_fm_voice n=0: ${fv.ops?.length} ops, ${fv.v?.length} values`);
const bad = JSON.parse(M.UTF8ToString(M._lab_fm_voice(0, 5, M.stringToNewUTF8('describe'), 0, 0, 0)));
check(bad.err === 'v needs 83 values' && !bad.v, `lab_fm_voice n=5: ${JSON.stringify(bad)}`);
const unk = JSON.parse(M.UTF8ToString(M._lab_fm_voice(0, 0, M.stringToNewUTF8('nope'), 0, 0, 0)));
check(typeof unk.err === 'string' && !unk.v, `lab_fm_voice unknown cmd: ${JSON.stringify(unk)}`);

// wt: the default parameters' table.
const WT_DEF = [0, 0, 0, 50, 50, 50, 10, 10, 10, 10, 10, 10, 0, 0, 0];
const meta = JSON.parse(M.UTF8ToString(M._lab_wt_meta(1)));
const vp = M._malloc(15), tp = M._malloc(meta.frames * meta.cycle * 2);
M.HEAPU8.set(WT_DEF, vp);
const scale = M._lab_wt_table(vp, 1, tp);
const table = M.HEAP16.slice(tp >> 1, (tp >> 1) + meta.frames * meta.cycle);
M._free(tp);
check(rms(table) > 0, `lab_wt_table default: ${meta.frames}x${meta.cycle}, rms ${rms(table).toFixed(0)}, scale ${scale.toFixed(3)}`);

// fm audio: arm, note on, 20 blocks.
const ap = M._malloc(16);
new Int32Array(M.HEAPU8.buffer, ap, 4).set([5, 250, 60, 200]);
M._lab_fm_arm(0, 0, ap);
M._free(ap);
M._lab_fm_note(60, 1);
const tp7 = M._malloc(28);
let live = 0;
const fmParts = [];
for (let b = 0; b < 20; b++) {
  fmParts.push(step(1).audio);
  live += M._lab_fm_trace_sample(tp7);
}
const ctrl = new Float32Array(M.HEAPU8.buffer, tp7, 7)[0];
check(rms(cat(fmParts)) > 0, `fm: arm + note on + 20 blocks: rms ${rms(cat(fmParts)).toFixed(1)}`);
check(live === 20 && ctrl > 0, `fm: trace samples live ${live}/20, ctrl ${ctrl.toFixed(3)}`);
M._lab_fm_note(60, 0);
step(64);
M._free(tp7);

// wt audio: arm, step until ready, note on, 20 blocks.
M._lab_wt_arm(vp, 1);
let waited = 0;
while (!M._lab_wt_ready() && waited < 64) { step(1); waited++; }
check(M._lab_wt_ready() === 1, `wt: ready after ${waited} blocks`);
M._lab_wt_note(48, 0.5, 1);
const wtAudio = cat(steps(20));
check(rms(wtAudio) > 0, `wt: arm + note on + 20 blocks: rms ${rms(wtAudio).toFixed(1)}`);

// The cost of 1-block stepping, timed on a 3 s sweep of the sounding note
// as page/lab.js renders it (the fm trace steps the same way).
const on = Math.trunc(3.0 * 48000 / 256);
const s0 = performance.now();
for (let b = 0; b < 2 * on; b++) {
  M._lab_wt_pos(b < on ? b / (on - 1) : (2 * on - 1 - b) / (on - 1));
  step(1);
}
const ms = performance.now() - s0;
console.log(`1-block step: ${(ms / (2 * on)).toFixed(3)} ms per block; a 3 s sweep (2 x ${on} blocks) ${ms.toFixed(0)} ms`);
M._lab_wt_note(48, 0, 0);
M._free(vp);

process.exit(failed ? 1 : 0);
