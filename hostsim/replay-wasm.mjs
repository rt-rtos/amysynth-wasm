#!/usr/bin/env node
// hostsim's WebAssembly module under node: replays a log the way
// `hostsim replay` does and writes the same outputs, so the two builds can be
// compared file by file.
//
//   node replay-wasm.mjs <module.js> <log> <outdir>
//
// <module.js> is a node build of the module (sim-out/hostsim-wasm/node or
// node-check). Writes out.wav, one <name>.txt per `shot`, final.txt,
// seqdump.txt and record.log. No PNGs, no project argument.
//
// Each block's lines run through hs_step(1, ...) at offset 0, which applies
// them before rendering the block, as replay does. A `shot` records the frame
// left by the blocks before it, so a shot that follows a command on the same
// block sees the frame before that command (native replay sees it after).
// The final seqdump runs as a command of one extra block whose audio is
// dropped.
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';

const [modPath, logPath, outDir] = process.argv.slice(2);
if (!outDir) {
  console.error('usage: node replay-wasm.mjs <module.js> <log> <outdir>');
  process.exit(2);
}
const require = createRequire(import.meta.url);
const createHostsim = require(path.resolve(modPath));

const lines = [];
for (const raw of fs.readFileSync(logPath, 'utf8').split('\n')) {
  const t = raw.trim();
  if (!t || t.startsWith('#')) continue;
  const m = /^@(\d+)\s+(.*)$/.exec(t);
  if (!m) { console.error(`${logPath}: expected '@<block> <command>': ${t}`); process.exit(2); }
  lines.push([Number(m[1]), m[2]]);
}

const modDir = path.dirname(path.resolve(modPath));
const M = await createHostsim({
  locateFile: f => path.join(modDir, f),
  print: () => {},
  printErr: t => process.stderr.write(t + '\n'),
});
M.FS.mkdir('/proj');
M._hs_boot();

const frameText = (name, bytes) => {
  const rows = [`# ${name} 128x64`];
  for (let y = 0; y < 64; y++) {
    let r = '';
    for (let x = 0; x < 128; x++) r += bytes[(y >> 3) * 128 + x] & (1 << (y & 7)) ? '#' : '.';
    rows.push(r);
  }
  return rows.join('\n') + '\n';
};
const currentFrame = () => M.HEAPU8.slice(M._hs_frame(), M._hs_frame() + 1024);
let record = '', consoleText = '';
const step = input => {
  const p = M.stringToNewUTF8(input.join('\n'));
  if (M._hs_step(1, p) !== 0) throw new Error('hs_step failed');
  M._free(p);
  const st = JSON.parse(M.UTF8ToString(M._hs_json(), M._hs_json_len()));
  record += st.record;
  consoleText += st.console;
  return M.HEAPU8.slice(M._hs_audio(), M._hs_audio() + M._hs_audio_len());
};

fs.mkdirSync(outDir, { recursive: true });
const end = lines.length ? lines[lines.length - 1][0] + Math.floor(48000 / 256) : 0;
const audio = [];
let i = 0, block = 0, ended = false;
while (!ended) {
  const input = [];
  for (; i < lines.length && lines[i][0] === block; i++) {
    const t = lines[i][1];
    let m;
    if ((m = /^shot (\S+)/.exec(t))) fs.writeFileSync(path.join(outDir, m[1] + '.txt'), frameText(m[1], currentFrame()));
    else if (t === 'end') ended = true;
    else if ((m = /^key (\d+) (down|up)/.exec(t))) input.push(`key 0 ${m[1]} ${m[2]}`);
    else if ((m = /^wheel (-?\d+)/.exec(t))) input.push(`enc 0 ${m[1]}`);
    else input.push(`cmd 0 ${t}`);
  }
  if (ended || (i === lines.length && block >= end)) break;
  audio.push(step(input));
  block++;
}

fs.writeFileSync(path.join(outDir, 'final.txt'), frameText('final', currentFrame()));
consoleText = '';
step(['cmd 0 st.seqdump']);
fs.writeFileSync(path.join(outDir, 'seqdump.txt'), consoleText);
fs.writeFileSync(path.join(outDir, 'record.log'), record);

const pcm = Buffer.concat(audio.map(a => Buffer.from(a)));
const hdr = Buffer.alloc(44);
hdr.write('RIFF', 0); hdr.writeUInt32LE(36 + pcm.length, 4); hdr.write('WAVEfmt ', 8);
hdr.writeUInt32LE(16, 16); hdr.writeUInt16LE(1, 20); hdr.writeUInt16LE(2, 22);
hdr.writeUInt32LE(48000, 24); hdr.writeUInt32LE(48000 * 4, 28); hdr.writeUInt16LE(4, 32);
hdr.writeUInt16LE(16, 34); hdr.write('data', 36); hdr.writeUInt32LE(pcm.length, 40);
fs.writeFileSync(path.join(outDir, 'out.wav'), Buffer.concat([hdr, pcm]));
console.error(`replayed ${lines.length} lines, ${block} blocks (${(block * 256 / 48000).toFixed(2)} s)`);
