/* The device page's in-browser backend: the firmware's UI and engine compiled
 * to WebAssembly run in this page. Same interface as the page's HTTP backend:
 * step(blocks, inputLines) resolves to a step answer, restart() boots a fresh
 * instance, log() is the recorded input since boot, about() a status line.
 * readProject(slot) and persist(), which the HTTP backend lacks, return a
 * saved project file's bytes and write /proj to IndexedDB at once.
 *
 * The module and its data are fetched once; a reboot instantiates them again
 * from memory, so nothing is fetched after the first load. Saved projects
 * live in the module filesystem's /proj, mirrored to IndexedDB every 2 s and
 * before a reboot; without IndexedDB they last until the page is closed.
 *
 * A lab page (wt/, fm/, lab.js) hands over a design on the BroadcastChannel
 * 'amysynth-lab': {kind: 'fm', v} or {kind: 'wt', v, range}. It goes into the
 * running firmware through lab.c's apply calls, between steps, and is
 * answered {kind: 'ack', of} (or 'nack' with a reason before the first boot);
 * saving it is the ordinary project save. */
'use strict';
window.EMU_WASM = (() => {
  const meta = name => (document.querySelector(`meta[name="${name}"]`) || {}).content || '';
  const version = meta('firmware-version');
  let mod = null, booting = null, assets = null, log = [], dirty = false, persistent = null;
  let booted = false, applied = '';
  const sync = populate => new Promise((ok, fail) => mod.FS.syncfs(populate, e => (e ? fail(e) : ok())));

  async function fetchAssets() {
    const get = async (f, optional) => {
      const r = await fetch(f);
      if (r.ok) return r.arrayBuffer();
      if (optional && r.status === 404) return null;
      throw new Error(`${f}: HTTP ${r.status}`);
    };
    const [wasm, data] = await Promise.all([get('hostsim.wasm'), get('hostsim.data', true)]);
    return { wasm, data };
  }

  /* The single-file build embeds the module and its data in hostsim.js. */
  const single = !!meta('single-file');

  async function boot() {
    booted = false;
    applied = '';
    const opts = { print: t => console.log(t), printErr: t => console.warn(t) };
    if (!single) {
      assets ||= await fetchAssets();
      opts.wasmBinary = assets.wasm;
      opts.getPreloadedPackage = () => (assets.data ? assets.data.slice(0) : null);
    }
    mod = await createHostsim(opts);
    mod.FS.mkdir('/proj');
    try {
      mod.FS.mount(mod.IDBFS, {}, '/proj');
      await sync(true);
      persistent = true;
    } catch (e) {
      console.warn('IndexedDB unavailable: projects last until the page is closed', e);
      persistent = false;
    }
    mod._hs_boot();
    booted = true;
    log = [];
  }

  setInterval(() => {
    if (mod && dirty && persistent) { dirty = false; sync(false).catch(e => console.warn(e)); }
  }, 2000);

  /* A message handler runs between steps: hs_step() is synchronous. */
  if (typeof BroadcastChannel !== 'undefined') {
    const lab = new BroadcastChannel('amysynth-lab');
    lab.onmessage = e => {
      const msg = e.data || {};
      if (msg.kind !== 'fm' && msg.kind !== 'wt') return;
      if (!mod || !booted) { lab.postMessage({ kind: 'nack', reason: 'power on the device first' }); return; }
      const v = Array.isArray(msg.v) ? msg.v.map(Number) : [];
      if (v.length !== (msg.kind === 'fm' ? 83 : 15)) { lab.postMessage({ kind: 'nack', reason: 'bad design' }); return; }
      if (msg.kind === 'fm') {
        const p = mod._malloc(v.length * 8);
        mod.HEAPF64.set(v, p >> 3);
        mod._lab_fm_apply(p, v.length);
        mod._free(p);
        applied = 'Applied the FM voice from the lab';
      } else {
        const p = mod._malloc(v.length);
        mod.HEAPU8.set(v.map(x => Math.max(0, Math.min(255, Math.round(x) || 0))), p);
        mod._lab_wt_apply(p, Math.max(0, Math.min(2, Math.round(Number(msg.range)) || 0)));
        mod._free(p);
        applied = 'Applied the wavetable from the lab';
      }
      const line = document.getElementById('about');
      if (line) line.textContent = about();
      lab.postMessage({ kind: 'ack', of: msg.kind });
    };
  }

  function about() {
    const where = persistent === null ? 'Projects: starting'
      : persistent ? 'Projects are saved in this browser (IndexedDB)'
      : 'Projects last until this page is closed (no IndexedDB)';
    return (version ? `Firmware ${version}. ` : '') + where + '.' + (applied ? ` ${applied}.` : '');
  }

  function answer() {
    const st = JSON.parse(mod.UTF8ToString(mod._hs_json(), mod._hs_json_len()));
    const fp = mod._hs_frame(), fl = mod._hs_frame_len();
    const ap = mod._hs_audio(), al = mod._hs_audio_len();
    st.frame = fl ? mod.HEAPU8.slice(fp, fp + fl) : null;
    st.audio = mod.HEAPU8.slice(ap, ap + al);
    st.restarted = false;
    log.push(...st.record.split('\n').filter(Boolean));
    dirty = true;
    return st;
  }

  const notices = document.getElementById('notices');
  if (notices) {
    notices.innerHTML = 'Runs the firmware\'s own code compiled to WebAssembly: menus, editors, sequencer and ' +
      'the AMY synthesis engine. Not timing-accurate: no CPU budget, memory limits or USB path. ' +
      (single ? '<details><summary>Licences and third-party notices</summary><pre id="notices-text"></pre></details>'
              : '<a href="notices.txt">Licences and third-party notices</a>.');
    if (single) document.getElementById('notices-text').textContent = document.getElementById('notices-txt').value;
    notices.hidden = false;
  }

  return {
    async step(blocks, lines) {
      if (!mod) await (booting ||= boot());
      const p = mod.stringToNewUTF8(lines.join('\n'));
      try {
        if (mod._hs_step(blocks, p) !== 0) throw new Error('hs_step: block count out of range');
      } finally {
        mod._free(p);
      }
      return answer();
    },
    async restart() {
      if (mod && persistent) { try { await sync(false); } catch (e) { console.warn(e); } }
      mod = null;
      booting = boot();
      await booting;
      return this.step(1, []);
    },
    async log() { return log.join('\n') + '\n'; },
    /* The slot file as the firmware's project store wrote it (Pnn.amp in the
     * module filesystem's /proj): the same bytes a board stores. */
    async readProject(slot) {
      if (!mod) throw new Error('not running');
      return mod.FS.readFile(`/proj/P${String(slot).padStart(2, '0')}.amp`);
    },
    /* Mirror /proj to IndexedDB now instead of on the next 2 s tick. */
    async persist() {
      if (mod && persistent) await sync(false);
    },
    about,
  };
})();
