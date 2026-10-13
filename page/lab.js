/* The lab pages' backend (wt/, fm/): an instance of the device's module of
 * their own, driven through the lab calls of hostsim/lab.c. It never mounts
 * /proj (the device tab's IndexedDB mirror is the only writer) and never
 * plays the device's audio; it boots on the first call, stops the
 * transport, and then steps only while a call renders, so between renders
 * it costs nothing. Calls run one at a time in the order they were made.
 *
 * apply() hands a design to the device tab over the BroadcastChannel
 * 'amysynth-lab', or, in a tab the single-file page wrote, by postMessage
 * to window.opener; wasm.js applies it there and answers. In that tab the
 * back link ([data-device-focus]) focuses the device tab. */
'use strict';
window.LAB = (() => {
  const RATE = 48000, BPS = RATE / 256;
  const FM_N = 83, WT_N = 15;
  const HOLD_S = 2.0, SWEEP_S = 3.0, WT_TAIL_S = 0.1, WT_READY_MAX = 64;
  const FM_TAIL_S = 0.3, FM_TAIL_MAX_S = 5.0;   /* after note-off: the control osc releases in 200 ms */
  /* hostsim halves the engine's output as the device does (hostsim.c), so
   * the engine's full scale lands here. */
  const FULL = 16383;
  let mod = null, booting = null, chain = Promise.resolve();

  /* The single-file page embeds the module and its data in hostsim.js. */
  const single = !!(document.querySelector('meta[name="single-file"]') || {}).content;

  for (const a of document.querySelectorAll('[data-device-focus]')) {
    a.addEventListener('click', e => {
      e.preventDefault();
      if (window.opener) window.opener.focus();
    });
  }

  async function boot() {
    const opts = { print: t => console.log(t), printErr: t => console.warn(t) };
    if (!single) {
      const get = async (f, optional) => {
        const r = await fetch(f);
        if (r.ok) return r.arrayBuffer();
        if (optional && r.status === 404) return null;
        throw new Error(`${f}: HTTP ${r.status}`);
      };
      const [wasm, data] = await Promise.all([get('../hostsim.wasm'), get('../hostsim.data', true)]);
      opts.wasmBinary = wasm;
      opts.getPreloadedPackage = () => (data ? data.slice(0) : null);
    }
    const m = await createHostsim(opts);
    m._hs_boot();
    mod = m;
    step(1, ['cmd 0 tr.stop', 'cmd 0 amy.wire V5']);   /* V5: the lab's listening level */
  }

  /* fn(mod) after the boot, after every call made before this one. */
  function run(fn) {
    const p = chain.then(async () => {
      if (!mod) await (booting ||= boot());
      return fn(mod);
    });
    chain = p.catch(() => {});
    return p;
  }

  /* Renders `blocks` blocks; resolves the step's audio, 16-bit stereo. */
  function step(blocks, lines = []) {
    const p = mod.stringToNewUTF8(lines.join('\n'));
    try {
      if (mod._hs_step(blocks, p) !== 0) throw new Error('hs_step: block count out of range');
    } finally {
      mod._free(p);
    }
    const ap = mod._hs_audio();
    return new Int16Array(mod.HEAPU8.slice(ap, ap + mod._hs_audio_len()).buffer);
  }

  /* fn(ptr, n) with vals copied into the module as uint8 (bytes) or double;
   * ptr is 0 for an empty list. */
  function withBytes(vals, fn) {
    const p = mod._malloc(Math.max(1, vals.length));
    try {
      mod.HEAPU8.set(vals, p);
      return fn(p, vals.length);
    } finally {
      mod._free(p);
    }
  }
  function withDoubles(vals, fn) {
    if (!vals.length) return fn(0, 0);
    const p = mod._malloc(vals.length * 8);
    try {
      mod.HEAPF64.set(vals, p >> 3);
      return fn(p, vals.length);
    } finally {
      mod._free(p);
    }
  }

  const list = v => Array.isArray(v) ? v : String(v ?? '').split(',').filter(s => s.trim() !== '');
  const clampInt = (x, lo, hi) => Math.max(lo, Math.min(hi, Math.round(Number(x)) || 0));
  const fmVals = v => list(v).map(Number);
  function wtVals(v) {
    const a = list(v).map(x => clampInt(x, 0, 255));
    if (a.length !== WT_N) throw new Error(`v needs ${WT_N} values`);
    return a;
  }
  const wtRange = r => clampInt(r ?? 1, 0, 2);

  /* A mono WAV of the left channel of the concatenated steps, and how many
   * samples (both channels) sit at full scale. */
  function wav(parts) {
    let n = 0, clipped = 0;
    for (const p of parts) n += p.length >> 1;
    const pcm = new Int16Array(n);
    let o = 0;
    for (const p of parts) {
      for (let i = 0; i < p.length; i++) if (p[i] >= FULL || p[i] <= -FULL - 1) clipped++;
      for (let i = 0; i < p.length; i += 2) pcm[o++] = p[i];
    }
    return { wav: wavBlob(pcm, RATE), clipped };
  }

  function voice(m, vals, cmd, args) {
    const a = list(args).map(x => Math.trunc(Number(x)) || 0);
    const cp = m.stringToNewUTF8(cmd || 'describe');
    try {
      const j = JSON.parse(withDoubles(vals, (p, n) => m.UTF8ToString(m._lab_fm_voice(p, n, cp, a[0] | 0, a[1] | 0, a[2] | 0))));
      if (j.err && !j.v) throw new Error(j.err);
      return j;
    } finally {
      m._free(cp);
    }
  }

  /* LAB_SYNTH set up with the voice and the row ADSR; the gate and tail in
   * blocks, as the desktop lab's render. */
  function fmArm(m, vals, gate, row) {
    if (vals.length !== 0 && vals.length !== FM_N) throw new Error(`v needs ${FM_N} values`);
    let g = Number(gate);
    if (!(g >= 0.05)) g = 0.05;
    if (g > 10) g = 10;
    const adsr = row && list(row).length === 4 ? list(row).map(x => Math.max(0, Math.round(Number(x)) || 0)) : null;
    let tail = FM_TAIL_S;
    if (adsr && adsr[3] / 1000 + 0.1 > tail) tail = adsr[3] / 1000 + 0.1;
    if (tail > FM_TAIL_MAX_S) tail = FM_TAIL_MAX_S;
    withDoubles(vals, (p, n) => {
      if (!adsr) return m._lab_fm_arm(p, n, 0);
      const ap = m._malloc(16);
      try {
        new Int32Array(m.HEAPU8.buffer, ap, 4).set(adsr);
        m._lab_fm_arm(p, n, ap);
      } finally {
        m._free(ap);
      }
    });
    return { on: Math.trunc(g * BPS + 0.5), tail: Math.trunc(tail * BPS + 0.5) };
  }

  const fmNote = n => clampInt(n, 0, 127);

  return {
    /* The table of the parameters: { table: Int16Array, meta }, meta with
     * the clamped params, the range and the whole-table scale. */
    wtTable(v15, range) {
      return run(m => {
        const vals = wtVals(v15), rng = wtRange(range);
        const meta = JSON.parse(m.UTF8ToString(m._lab_wt_meta(rng)));
        const n = meta.frames * meta.cycle, tp = m._malloc(n * 2);
        try {
          return withBytes(vals, vp => {
            meta.scale = m._lab_wt_table(vp, rng, tp);
            meta.params = Array.from(m.HEAPU8.subarray(vp, vp + WT_N));
            meta.range = rng;
            return { table: m.HEAP16.slice(tp >> 1, (tp >> 1) + n), meta };
          });
        } finally {
          m._free(tp);
        }
      });
    },
    /* The 15 values with M set to the blend of A and B. */
    wtBlend(v15) {
      return run(m => withBytes(wtVals(v15), vp => {
        m._lab_wt_blend(vp);
        return Array.from(m.HEAPU8.subarray(vp, vp + WT_N));
      }));
    },
    /* One note of the table: held at frame position pos (0..frames-1), or
     * (mode 'sweep') swept first frame -> last -> first. { wav, clipped }. */
    wtRender(v15, range, note, pos, mode) {
      return run(m => {
        const vals = wtVals(v15), rng = wtRange(range), n = fmNote(note);
        const frames = JSON.parse(m.UTF8ToString(m._lab_wt_meta(rng))).frames;
        const at = Math.max(0, Math.min(frames - 1, Number(pos) || 0)) / (frames - 1);
        withBytes(vals, vp => m._lab_wt_arm(vp, rng));
        for (let b = 0; b < WT_READY_MAX && !m._lab_wt_ready(); b++) step(1);
        const parts = [];
        if (mode === 'sweep') {
          const on = Math.trunc(SWEEP_S * BPS);
          m._lab_wt_note(n, 0, 1);
          for (let b = 0; b < 2 * on; b++) {
            m._lab_wt_pos(b < on ? b / (on - 1) : (2 * on - 1 - b) / (on - 1));
            parts.push(step(1));
          }
        } else {
          m._lab_wt_note(n, at, 1);
          parts.push(step(Math.max(2, Math.trunc(HOLD_S * BPS))));
        }
        m._lab_wt_note(n, 0, 0);
        parts.push(step(Math.trunc(WT_TAIL_S * BPS)));
        return wav(parts);
      });
    },
    /* The voice after one command (lab.c: lab_fm_voice); v empty is the
     * default voice. A refused request rejects with the message. */
    fmVoice(v, cmd, args) {
      return run(m => voice(m, fmVals(v), cmd, args));
    },
    /* One note: note-on, gate seconds, the release. { wav, clipped }. */
    fmRender(v, note, gate, row) {
      return run(m => {
        const vals = fmVals(v), n = fmNote(note);
        const { on, tail } = fmArm(m, vals, gate, row);
        m._lab_fm_note(n, 1);
        const parts = [step(on)];
        m._lab_fm_note(n, 0);
        parts.push(step(tail));
        return wav(parts);
      });
    },
    /* The same note as fmRender as AMY's envelope values per block: the
     * `trace` object (linear amplitude, 1 = one carrier at full level). */
    fmTrace(v, note, gate, row) {
      return run(m => {
        const vals = fmVals(v), n = fmNote(note);
        const desc = voice(m, vals, 'describe');
        const { on, tail } = fmArm(m, vals, gate, row), total = on + tail;
        const rows = [], sp = m._malloc(7 * 4);
        try {
          m._lab_fm_note(n, 1);
          for (let b = 0; b < total; b++) {
            if (b === on) m._lab_fm_note(n, 0);
            step(1);
            m._lab_fm_trace_sample(sp);
            rows.push(Float32Array.from(new Float32Array(m.HEAPU8.buffer, sp, 7)));
          }
        } finally {
          m._free(sp);
        }
        const out = desc.ops.map(o => o.targets), ob = desc.out_bit;
        const tr = { dt: 256 / RATE, on, n: total, ops: [] };
        for (let i = 0; i < 6; i++) tr.ops.push(rows.map(r => r[1 + i]));
        tr.ctrl = rows.map(r => r[0]);
        tr.voice = rows.map(r => out.reduce((s, t, i) => s + (t & ob ? r[1 + i] : 0), 0));
        tr.loud = rows.map((r, b) => r[0] * tr.voice[b]);
        tr.bright = rows.map(r => out.reduce((s, t, i) => s + (t && !(t & ob) ? r[1 + i] : 0), 0));
        tr.peak = {};
        for (const k of ['ctrl', 'voice', 'loud', 'bright']) tr.peak[k] = tr[k].reduce((a, x) => Math.max(a, x), 0);
        return tr;
      });
    },
    /* The voice as an fm_voice_t initializer, text. */
    fmInit(v) {
      return run(m => {
        const text = withDoubles(fmVals(v), (p, n) => m.UTF8ToString(m._lab_fm_init(p, n)));
        if (text.startsWith('{"err"')) throw new Error(JSON.parse(text).err);
        return text;
      });
    },
    /* Send the design to the device tab: kind 'fm' with payload { v }, or
     * 'wt' with { v, range }. Resolves true when a device tab applied it
     * within 1 s, false when none answered; rejects with the device tab's
     * reason when it could not. */
    apply(kind, payload) {
      if (single && window.opener) {
        const dev = window.opener;
        return new Promise((resolve, reject) => {
          const done = (fn, x) => { clearTimeout(timer); window.removeEventListener('message', on); fn(x); };
          const timer = setTimeout(() => done(resolve, false), 1000);
          const on = e => {
            if (e.source !== dev) return;
            const msg = e.data || {};
            if (msg.kind === 'ack' && msg.of === kind) done(resolve, true);
            else if (msg.kind === 'nack') done(reject, new Error(msg.reason || 'the device tab refused'));
          };
          window.addEventListener('message', on);
          dev.postMessage({ ...payload, kind }, '*');
        });
      }
      if (typeof BroadcastChannel === 'undefined')
        return Promise.reject(new Error('Apply needs BroadcastChannel, which this browser lacks; the URL hash keeps the design'));
      const ch = new BroadcastChannel('amysynth-lab');
      return new Promise((resolve, reject) => {
        const done = (fn, x) => { clearTimeout(timer); ch.close(); fn(x); };
        const timer = setTimeout(() => done(resolve, false), 1000);
        ch.onmessage = e => {
          const msg = e.data || {};
          if (msg.kind === 'ack' && msg.of === kind) done(resolve, true);
          else if (msg.kind === 'nack') done(reject, new Error(msg.reason || 'the device tab refused'));
        };
        ch.postMessage({ ...payload, kind });
      });
    },
  };
})();
