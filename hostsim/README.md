# hostsim - the device's UI and engine on the host

The firmware's engine (`components/synth_core`, the project store) and its
UI layer: every `synth_ui` source, the `components/display` renderers,
`main/input_dispatch.c` and the harness interpreter (`harness_exec()`),
linked against the same fixed-point 48 kHz AMY. One thread, no clock of its
own: it advances only when a replay log or a step request tells it to.

    ./build.sh                                    # -> out/native/hostsim
    out/native/hostsim replay <log> <outdir> [project.amp]
    out/native/hostsim serve [workdir]            # the step protocol, below

`./build.sh san` builds with ASan and UBSan into `out/san` (run with
`ASAN_OPTIONS=detect_leaks=0`).

## What runs, per 256-frame block

1. Button timers: `LONG_PRESS_START` after a 1500 ms hold, `SINGLE_CLICK`
   180 ms after a short press's release (iot_button's defaults, which
   `my_buttons.c` keeps). A second press inside the 180 ms makes a double
   click, which `main.c` drops, so it produces no click.
2. Encoder: detents gathered since the last poll go to
   `input_dispatch_encoder_steps()` as one call every 20 ms (`encoder_task`).
3. `synth_ui_slice(phase)` for every 10 ms of audio time that has passed, in
   phase order.
4. The block: ingest pump, `amy_execute_deltas()` (the sequencer tick hook),
   pump, `amy_render()`, the output halved (below), then the render-task hooks `main.c` runs after
   `amy_update()` (sample recorder, loop bounce, drum cache, filter scope).
   `xTaskGetCurrentTaskHandle()` reads as the render task here, so the
   ingest helpers' render-path asserts are live.

Time is audio time: `xTaskGetTickCount()` (1 ms ticks) and
`esp_timer_get_time()` count rendered frames. Boot follows `app_main()`:
drums partition, AMY config, wavetable bank, project store, `synth_ui_init()`.
Same boot, same input, same output: two replays of one log give identical
WAV, frames and dumps.

Output level: AMY's `ESP_PLATFORM` branch halves every sample after the soft
clip (`amy.c`); the host build does not. hostsim halves the block the same
way, so its WAV, the page's audio, the sample recorder, the loop bounce and
the output watchdog all see the device's level.

The output watchdog (`usb_audio_watchdog.c`) is the firmware's; hostsim
gives it `usb_audio_peek_levels()` over a ring of the last rendered output,
so the CLIP and LOUD badges come and go as on the device.

Stood in for (beside the headers in `stubs/`; all of it in `../SHIMS.md`):
the panel (`display_flush()` keeps the u8g2 buffer, `i2c_u8g2_service()`
returns what a present panel returns), `core_load_last()` and
`dropout_stats_get()` (no data).
The host Kconfig is `config/sdkconfig.h`, the device's with
`CONFIG_SYNTH_WIRELESS` cleared: no BLE page, badge or live-play voice. The
template table is generated from the template specs as the firmware build
does. `host/host_glue.c` hands the `seq_ui` task handle back as the main
thread, which makes the step loop the layers applier `synth_ui_init()`
registers.

Not covered: Core 0 / Core 1 interleaving (input and UI run between blocks,
never during one), USB, real render cost, button bounce, the panel's
transfer time.

## Replay log

One command per line, `@<block> <command>`, blocks counted from the end of
boot and never decreasing. `#` starts a comment line.

| Command | Effect |
|---|---|
| any harness command | through `harness_exec()`: `in.btn id=<n> act=down\|up\|click\|long`, `in.enc delta=<n>`, `tr.play`, `tr.stop`, `tr.bpm <n>`, `st.seqdump`, ... |
| `key <id> down\|up` | a raw button edge, through the timers above |
| `wheel <n>` | raw encoder detents, through the 20 ms poll |
| `shot <name>` | write the current frame as `<name>.png` and `<name>.txt` |
| `end` | stop here |

Without `end` the replay stops one second after the last line. Button ids are
`my_button_id_t`: 0 SHOULDER, 1, 2, 3, 4 encoder click, 5 button 0, 6 SHIFT.

Outputs in `<outdir>`, which is also the working directory (projects go to
`<outdir>/proj`, emptied at start): `out.wav` (from block 0), the shots,
`final.png` / `final.txt`, `seqdump.txt` (`st.seqdump` at the end) and
`record.log`. `[project.amp]` is loaded into slot 0 after boot, before
block 0.

`record.log` is the dispatcher-level form of the input: every button event
and encoder call `input_dispatch_*` received, plus state-changing harness
commands, each at its block. Raw `key` and `wheel` lines appear there as the
events they turned into. A log in this form replays to the same result and
can be sent to the board over the serial harness.

## serve: the step protocol

Requests on stdin, one at a time:

    step <blocks>
    key <offset> <id> down|up
    enc <offset> <detents>
    cmd <offset> <harness command>
    .

`<offset>` is the block within the step at which the input applies (clamped
to the step). `quit` ends the program. Each request is answered on stdout:

    ok <json_bytes> <frame_bytes> <audio_bytes>\n<json><frame><audio>

- json: `{"block": <blocks rendered since boot>, "first": <block the step
  started at>, "ms": <audio time>, "view": <synth_ui_active_view() after
  the step, a ui_view_id_t>, "load": [<mean>, <max>] (the cost model of
  `../load_model.c` over the step's blocks, % of the 5.33 ms block; relative,
  not a measurement), "console": <harness replies and dump
  output>, "record": <log lines the step recorded>}`
- frame: the 1024-byte u8g2 buffer (128 x 64, `vertical_top_lsb`: byte
  `(y >> 3) * 128 + x`, bit `y & 7`) when a UI slice flushed since the last
  answer, else 0 bytes.
- audio: the step's blocks, 16-bit stereo interleaved, 48 kHz.

A malformed request is answered `err ...`. Anything the firmware prints to
stdout outside a command goes to stderr.

## WebAssembly: the device in the browser

    source <emsdk>/emsdk_env.sh
    ./build.sh wasm                                  # -> out/wasm/site
    (cd out/wasm/site && python3 -m http.server)     # open index.html

The same sources through emcc, for static hosting (GitHub Pages: no response
headers, so no threads and no SharedArrayBuffer). The site is self-contained,
all URLs relative:

| File | What |
|---|---|
| `index.html` | `page/index.html`, with the version in a meta tag and the in-page backend loaded. It opens on a template picker built from what `pr.tpl` lists; a pick starts the audio, queues the load and presses play 200 ms later, after the UI task has applied it |
| `wasm.js` | the in-page backend: same `step(blocks, events)` as the page's HTTP backend, which talks to a server running `hostsim serve` (no such server is in this repo) |
| `hostsim.js`, `hostsim.wasm` | the module; exports `hs_boot`, `hs_step` and getters for the answer |
| `hostsim.data` | the drum banks (`tools/gen-drums.py`), preloaded at `/components/amy/drums.bin` |
| `controls.md`, `notices.txt` | CONTROLS.md; the attributions and the licence texts of what is compiled in |

How it differs from the native build:
- AMY is compiled with `__EMSCRIPTEN__` undefined, so it takes the same path
  as the native host build instead of AMY's own web build (AudioWorklet hooks,
  a wasm-worker lock). Its audio backend (`libminiaudio-audio.c`) is left out
  (`AMY_NO_MINIAUDIO`) and its MIDI device layer is replaced by no-ops
  (`AMY_HOST_MIDI`); `hostsim.c` supplies the empty platform init.
- The page calls `hs_step()` directly; there is no stdin protocol.
- Projects: `/proj` in the module filesystem, mirrored to the browser's
  IndexedDB every 2 s and before a reboot; without IndexedDB they last until
  the page closes. The page says which.
- The module and its data are fetched once; a reboot instantiates them again
  from memory.
- Shadow stack (`-sSTACK_SIZE`): 1 MB. The test logs overflow at 8 KB (first
  overflow at 8,352 bytes) and pass at 16 KB. Scalar locals live on the wasm
  engine's own stack, which this does not measure.

`./build.sh wasm --single` also writes
`out/wasm/S3-Amysynth-<version>.html`: the whole site in one file
that opens from disk (file://), for passing around before anything is hosted.
The module is linked with `-sSINGLE_FILE` (base64: the default UTF-8 binary
encoding would put NUL bytes into an inline script, which HTML parsing
replaces) and `--embed-file` for drums.bin; `hostsim/single_file.py` inlines
the stylesheet and scripts and carries CONTROLS.md and the notices as hidden
textareas, since a file:// page cannot fetch. About 7.0 MB, 4.4 MB gzipped.

Assert messages name their source file (`__FILE__`); the wasm build maps
paths to ones relative to the firmware or to this repo
(`-ffile-prefix-map`), so no local path ships.

The build also links `out/wasm/node` (the module for node) and
`node-check` (wasm assertions, `SAFE_HEAP`, stack overflow checks).
`hostsim/replay-wasm.mjs <module.js> <log> <outdir>` replays a log through it
and writes out.wav, the shots' `.txt` frames, final.txt, seqdump.txt and
record.log, as `hostsim replay` does, for a file-by-file comparison. Shots
see the frame before any command on the same block; there is no project
argument.
