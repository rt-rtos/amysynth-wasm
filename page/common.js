/* Shared helpers of the lab pages: DOM and canvas setup, note names, WAV
 * in and out, the server-rendered audio player, an FFT. No model knowledge. */
'use strict';
const $ = id => document.getElementById(id);
const NOTE = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const noteName = m => NOTE[m % 12] + (Math.floor(m / 12) - 1);

/* A canvas sized to its CSS box at the device pixel ratio, cleared.
 * Returns the 2d context and the CSS-pixel width and height. */
function ctxOf(id) {
  const c = $(id), dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth, h = c.clientHeight;
  if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(h * dpr); }
  const g = c.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  g.font = '11px ui-monospace, Menlo, Consolas, monospace';
  return { g, w, h };
}

/* 0..1 -> dark blue, magenta, yellow: the colour scale of level maps. */
function heat(t) {
  t = Math.max(0, Math.min(1, t));
  const r = Math.round(255 * Math.min(1, t * 1.6)), gr = Math.round(255 * Math.max(0, t * 1.7 - 0.7));
  const b = Math.round(255 * (t < 0.5 ? 0.25 + t : 0.75 - (t - 0.5) * 1.5));
  return `rgb(${r},${gr},${Math.max(0, b)})`;
}

/* Mono 16-bit WAV from an Int16Array. clm, when given, is the text of a
 * `clm ` chunk (the wavetable frame size as Serum and Vital read it). */
function wavBlob(pcm, rate, clm) {
  const pad = clm ? clm.length & 1 : 0, clmLen = clm ? 8 + clm.length + pad : 0;
  const buf = new ArrayBuffer(44 + clmLen + pcm.length * 2), d = new DataView(buf);
  const tag = (o, t) => { for (let i = 0; i < 4; i++) d.setUint8(o + i, t.charCodeAt(i)); };
  tag(0, 'RIFF'); d.setUint32(4, buf.byteLength - 8, true); tag(8, 'WAVE');
  tag(12, 'fmt '); d.setUint32(16, 16, true); d.setUint16(20, 1, true); d.setUint16(22, 1, true);
  d.setUint32(24, rate, true); d.setUint32(28, rate * 2, true); d.setUint16(32, 2, true); d.setUint16(34, 16, true);
  let o = 36;
  if (clm) {
    tag(o, 'clm '); d.setUint32(o + 4, clm.length, true);
    for (let i = 0; i < clm.length; i++) d.setUint8(o + 8 + i, clm.charCodeAt(i));
    o += clmLen;
  }
  tag(o, 'data'); d.setUint32(o + 4, pcm.length * 2, true);
  for (let i = 0; i < pcm.length; i++) d.setInt16(o + 8 + i * 2, pcm[i], true);
  return new Blob([buf], { type: 'audio/wav' });
}

/* The samples of a 16-bit PCM WAV: { rate, channels, data } with data an
 * Int16Array, interleaved when channels > 1. */
function wavPcm(buf) {
  const d = new DataView(buf);
  let rate = 0, channels = 1, data = new Int16Array(0);
  for (let o = 12; o + 8 <= d.byteLength;) {
    const id = String.fromCharCode(d.getUint8(o), d.getUint8(o + 1), d.getUint8(o + 2), d.getUint8(o + 3));
    const len = d.getUint32(o + 4, true);
    if (id === 'fmt ') { channels = d.getUint16(o + 10, true); rate = d.getUint32(o + 12, true); }
    if (id === 'data') {
      const n = Math.min(len, d.byteLength - o - 8) >> 1;
      data = new Int16Array(n);
      for (let i = 0; i < n; i++) data[i] = d.getInt16(o + 8 + i * 2, true);
    }
    o += 8 + len + (len & 1);
  }
  return { rate, channels, data };
}

function saveBlob(blob, name) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

/* Fetch a WAV the server rendered, play it in ui.player and point the link
 * ui.save at it under the file name ui.name. Returns { res, buf } (the
 * Response for its headers, the WAV bytes), or null after putting the
 * server's error text in ui.msg. */
let labWavUrl = null;
async function playWav(url, ui) {
  const res = await fetch(url);
  if (!res.ok) { ui.msg.textContent = await res.text(); return null; }
  const buf = await res.arrayBuffer();
  if (labWavUrl) URL.revokeObjectURL(labWavUrl);
  labWavUrl = URL.createObjectURL(new Blob([buf], { type: 'audio/wav' }));
  ui.player.src = labWavUrl;
  ui.player.play().catch(() => {});
  ui.save.href = labWavUrl;
  ui.save.download = ui.name;
  ui.save.hidden = false;
  return { res, buf };
}

/* In-place radix-2 FFT of re/im (same power-of-two length). */
function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const a = i + k, b = a + len / 2;
        const xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
        const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}
