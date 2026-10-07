/* The device page's in-browser backend: the firmware's UI and engine compiled
 * to WebAssembly run in this page. Same interface as the page's HTTP backend:
 * step(blocks, inputLines) resolves to a step answer, restart() boots a fresh
 * instance, log() is the recorded input since boot, about() a status line.
 *
 * The module and its data are fetched once; a reboot instantiates them again
 * from memory, so nothing is fetched after the first load. Saved projects
 * live in the module filesystem's /proj, mirrored to IndexedDB every 2 s and
 * before a reboot; without IndexedDB they last until the page is closed. */
'use strict';
window.EMU_WASM = (() => {
  const meta = name => (document.querySelector(`meta[name="${name}"]`) || {}).content || '';
  const version = meta('firmware-version');
  let mod = null, booting = null, assets = null, log = [], dirty = false, persistent = null;
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
    log = [];
  }

  setInterval(() => {
    if (mod && dirty && persistent) { dirty = false; sync(false).catch(e => console.warn(e)); }
  }, 2000);

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
    about() {
      const where = persistent === null ? 'Projects: starting'
        : persistent ? 'Projects are saved in this browser (IndexedDB)'
        : 'Projects last until this page is closed (no IndexedDB)';
      return (version ? `Firmware ${version}. ` : '') + where + '.';
    },
  };
})();
