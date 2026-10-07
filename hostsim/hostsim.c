/* hostsim - the firmware's UI, input dispatcher and engine on the host, one
 * thread, advanced only when told to. Boots like main.c (wireless off), then
 * per 256-frame block: applies the input that is due, runs the button and
 * encoder timing the device's drivers have, renders the block, and calls
 * synth_ui_slice() every SYNTH_UI_SLICE_MS of audio time in the device's
 * phase order. Time is audio time: xTaskGetTickCount() and
 * esp_timer_get_time() count rendered frames.
 *
 *   hostsim replay <log> <outdir> [project.amp]
 *   hostsim serve [workdir]
 *
 * Grammar, protocol and outputs: README.md beside this file.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "amy.h"
#include "u8g2.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "my_buttons.h"
#include "input_dispatch.h"
#include "harness.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "seq_core_config.h"
#include "synth_slots.h"
#include "fx_bus.h"
#include "amy_helpers.h"
#include "filter_scope.h"
#include "custompatches/drum_cache.h"
#include "custompatches/wavetable_bank.h"
#include "custompatches/sample_rec.h"
#include "custompatches/clip_bounce.h"
#if CONFIG_SYNTH_CUSTOM_WT
#include "custompatches/wt_builder.h"
#endif
#include "project_fs.h"
#include "project_store.h"
#include "project_snapshot.h"
#include "usb_audio_watchdog.h"
#include "dropout_stats.h"
#include "core_load.h"
#include "frame_out.h"
#include "usb_audio.h"
#include "load_model.c"

extern int host_drums_load(const char *path);
extern esp_partition_t g_host_drums_part;
extern void host_pump_drain(void);
extern int g_host_log;
extern TaskHandle_t g_host_cur_task;

#define BLOCK_FRAMES       AMY_BLOCK_SIZE
#define LONG_PRESS_MS      1500u   /* iot_button defaults (my_buttons.c) */
#define SHORT_PRESS_MS     180u
#define ENCODER_POLL_MS    20u     /* main.c encoder_task */

/* ── Audio-time clock ────────────────────────────────────────────────── */

static uint64_t s_frames;          /* frames rendered since boot */
static uint32_t s_block;           /* blocks rendered since the log's @0 */

static uint32_t now_ms(void) { return (uint32_t)(s_frames * 1000u / AMY_SAMPLE_RATE); }
TickType_t xTaskGetTickCount(void) { return (TickType_t)now_ms(); }
int64_t esp_timer_get_time(void) { return (int64_t)(s_frames * 1000000u / AMY_SAMPLE_RATE); }

/* ── Device surface the UI reaches, stood in for ─────────────────────── */

static u8g2_t s_u8g2;
static bool s_frame_dirty;

bool i2c_u8g2_service(void) { return false; }
void display_flush(u8g2_t *u8g2) { (void)u8g2; s_frame_dirty = true; }
void display_flush_invalidate(void) { }

/* The USB ring's level peek, over the last rendered output (the output
 * watchdog polls it from the UI slice and drives the CLIP / LOUD badge). */
#define OUT_RING 8192u
static int16_t s_out_ring[OUT_RING];
static size_t s_out_idx;

bool usb_audio_peek_levels(size_t n_samples, int32_t *peak_abs,
                           int32_t *mean_abs, size_t *write_idx)
{
    if (n_samples == 0) return false;
    if (n_samples > OUT_RING / 4) n_samples = OUT_RING / 4;
    int32_t peak = 0, acc = 0;
    for (size_t i = 0; i < n_samples; ++i) {
        int32_t v = s_out_ring[(s_out_idx + OUT_RING - n_samples + i) % OUT_RING];
        if (v < 0) v = -v;
        acc += v;
        if (v > peak) peak = v;
    }
    *peak_abs = peak;
    *mean_abs = acc / (int32_t)n_samples;
    *write_idx = s_out_idx;
    return true;
}
bool core_load_last(uint8_t busy_pct[CORE_LOAD_NUM_CORES]) { (void)busy_pct; return false; }
void dropout_stats_get(dropout_stats_t *out) { memset(out, 0, sizeof *out); }

/* ── Text sinks: console replies and the recorded log ────────────────── */

typedef struct { char *buf; size_t len, cap; } sbuf_t;
static sbuf_t s_console, s_record;

static void sb_add(sbuf_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *nb = realloc(b->buf, cap);
        if (!nb) return;
        b->buf = nb; b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void sb_printf(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sbuf_t *b, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) sb_add(b, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

/* Firmware code prints dumps to stdout. The protocol owns the real stdout, so
 * fd 1 points at stderr except while a command runs, when it points at a
 * scratch file that lands in the console text. */
static int s_capture = -1;

static void capture_begin(void)
{
    fflush(stdout);
    if (s_capture < 0) {
        FILE *t = tmpfile();
        if (!t) return;
        s_capture = dup(fileno(t));
        fclose(t);
        if (s_capture < 0) return;
    }
    if (ftruncate(s_capture, 0) != 0 || lseek(s_capture, 0, SEEK_SET) != 0) return;
    dup2(s_capture, 1);
}

static void capture_end(void)
{
    fflush(stdout);
    dup2(2, 1);
    if (s_capture < 0) return;
    off_t n = lseek(s_capture, 0, SEEK_CUR);
    if (n <= 0 || lseek(s_capture, 0, SEEK_SET) != 0) return;
    char tmp[4096];
    ssize_t got;
    while (n > 0 && (got = read(s_capture, tmp, sizeof tmp)) > 0) {
        sb_add(&s_console, tmp, (size_t)got);
        n -= got;
    }
}

/* ── Input: dispatcher-level events, recorded as harness lines ───────── */

static void dispatch_button(int id, harness_btn_act_t act)
{
    static const char *names[] = { "down", "up", "click", "long" };
    static const button_event_t ev[] = { BUTTON_PRESS_DOWN, BUTTON_PRESS_UP,
                                         BUTTON_SINGLE_CLICK, BUTTON_LONG_PRESS_START };
    if (id < 0 || id >= MY_BUTTON_MAX || (unsigned)act > HARNESS_BTN_LONG) return;
    sb_printf(&s_record, "@%u in.btn id=%d act=%s\n", s_block, id, names[act]);
    input_dispatch_button((my_button_id_t)id, ev[act]);
}

static void dispatch_encoder(long steps)
{
    sb_printf(&s_record, "@%u in.enc delta=%ld\n", s_block, steps);
    input_dispatch_encoder_steps(steps);
}

static const harness_hooks_t s_hooks = {
    .inject_button        = dispatch_button,
    .inject_encoder_steps = dispatch_encoder,
};

/* Replies wait here until the command's dump output has been collected, so
 * the H< line follows the dump as it does on the serial harness. */
static sbuf_t s_reply;

static void reply_to_console(const char *line)
{
    sb_add(&s_reply, line, strlen(line));
    sb_add(&s_reply, "\n", 1);
}

/* One harness command line. Commands that only read state are not recorded;
 * in.btn / in.enc record themselves through the hooks. */
static void run_command(const char *line)
{
    while (*line == ' ') line++;
    if (*line == '\0') return;
    if (strncmp(line, "in.", 3) != 0 && strncmp(line, "st.", 3) != 0 &&
        strncmp(line, "sys.", 4) != 0) {
        sb_printf(&s_record, "@%u %s\n", s_block, line);
    }
    s_reply.len = 0;
    capture_begin();
    harness_exec(&s_hooks, line, reply_to_console);
    capture_end();
    if (s_reply.len) sb_add(&s_console, s_reply.buf, s_reply.len);
}

/* ── Raw keys -> button events, with iot_button's timing ─────────────────
 * PRESS_DOWN and PRESS_UP as they happen; LONG_PRESS_START after a hold of
 * LONG_PRESS_MS; SINGLE_CLICK SHORT_PRESS_MS after a short press's release
 * when no second press followed. A second press inside that window makes a
 * double click, which main.c drops, so it produces no click event. */

typedef struct {
    bool down, long_fired, click_pending;
    uint8_t clicks;
    uint32_t down_ms, up_ms;
} key_state_t;
static key_state_t s_keys[MY_BUTTON_MAX];
static long s_enc_accum;
static uint32_t s_enc_next_ms;

static void key_event(int id, bool down)
{
    if (id < 0 || id >= MY_BUTTON_MAX) return;
    key_state_t *k = &s_keys[id];
    uint32_t t = now_ms();
    if (down) {
        if (k->down) return;
        k->down = true; k->long_fired = false; k->down_ms = t;
        k->clicks = k->click_pending ? (uint8_t)(k->clicks + 1) : 1;
        k->click_pending = false;
        dispatch_button(id, HARNESS_BTN_DOWN);
    } else {
        if (!k->down) return;
        k->down = false;
        dispatch_button(id, HARNESS_BTN_UP);
        if (k->long_fired) k->clicks = 0;
        else { k->click_pending = true; k->up_ms = t; }
    }
}

static void input_timers(void)
{
    uint32_t t = now_ms();
    for (int id = 0; id < MY_BUTTON_MAX; id++) {
        key_state_t *k = &s_keys[id];
        if (k->down && !k->long_fired && t - k->down_ms >= LONG_PRESS_MS) {
            k->long_fired = true;
            k->clicks = 0;
            dispatch_button(id, HARNESS_BTN_LONG);
        }
        if (k->click_pending && t - k->up_ms >= SHORT_PRESS_MS) {
            k->click_pending = false;
            if (k->clicks == 1) dispatch_button(id, HARNESS_BTN_CLICK);
            k->clicks = 0;
        }
    }
    /* The device polls the encoder every ENCODER_POLL_MS and hands the
     * detents gathered since as one call. */
    if (t >= s_enc_next_ms) {
        s_enc_next_ms = t + ENCODER_POLL_MS;
        if (s_enc_accum != 0) {
            long steps = s_enc_accum;
            s_enc_accum = 0;
            dispatch_encoder(steps);
        }
    }
}

/* ── One block ───────────────────────────────────────────────────────── */

static TaskHandle_t s_ui_task;
static const TaskHandle_t s_render_task = (TaskHandle_t)0x3;
static uint32_t s_ui_next_ms;
static uint8_t s_ui_phase;

/* Cost-model load over the blocks since the last step answer. */
static double s_load_sum, s_load_max;
static uint32_t s_load_n;

static void seq_tick_hook(uint32_t tick) { (void)tick; sequencer_core_service_tick(); }

/* Renders one block into out (BLOCK_FRAMES stereo frames), or discards it. */
static void step_block(int16_t *out)
{
    input_timers();
    host_pump_drain();
    while ((int32_t)(now_ms() - s_ui_next_ms) >= 0) {
        synth_ui_slice(s_ui_phase);
        s_ui_phase = (uint8_t)((s_ui_phase + 1) % SYNTH_UI_SLICES_PER_FRAME);
        s_ui_next_ms += SYNTH_UI_SLICE_MS;
        host_pump_drain();
    }

    g_host_cur_task = s_render_task;
    amy_execute_deltas();
    g_host_cur_task = s_ui_task;
    host_pump_drain();
    g_host_cur_task = s_render_task;
    amy_render(0, amy_global.config.max_oscs, 0);
    int16_t *b = amy_fill_buffer();
    /* AMY's ESP_PLATFORM branch halves each sample after the soft clip; the
     * host build does not. Halve here so everything downstream (recorder,
     * bounce, watchdog, the audio out) sees the device's output. */
    for (int i = 0; i < BLOCK_FRAMES * AMY_NCHANS; i++) {
        int v = b[i];
        b[i] = (int16_t)(v < 0 ? -((-v) >> 1) : v >> 1);
        s_out_ring[s_out_idx] = b[i];
        s_out_idx = (s_out_idx + 1) % OUT_RING;
    }
    double cyc = load_model_block_cycles();
    s_load_sum += cyc;
    if (cyc > s_load_max) s_load_max = cyc;
    s_load_n++;
    sample_rec_render_tick(b, BLOCK_FRAMES);
    clip_bounce_render_tick(b, BLOCK_FRAMES);
    drum_cache_render_service();
#if CONFIG_FILTER_SCOPE
    filter_scope_render_tick();
#endif
    g_host_cur_task = s_ui_task;
    if (out) memcpy(out, b, BLOCK_FRAMES * AMY_NCHANS * sizeof(int16_t));
    s_frames += BLOCK_FRAMES;
    s_block++;
}

/* ── Boot: main.c's app_main order, minus USB, panel and radio ───────── */

static void boot(void)
{
    const char *env_log = getenv("DEVSIM_LOG");
    if (env_log) g_host_log = atoi(env_log);
    if (!host_drums_load(HOSTSIM_DRUMS))
        fprintf(stderr, "warn: drums.bin not loaded\n");

    u8g2_Setup_ssd1315_i2c_128x64_noname_f(&s_u8g2, U8G2_R0, u8x8_byte_empty, u8x8_dummy_cb);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);

    amy_config_t cfg = amy_default_config();
    cfg.audio = AMY_AUDIO_IS_NONE;
    cfg.platform.multicore = 0;
    cfg.platform.multithread = 0;
    cfg.amy_external_sequencer_hook = seq_tick_hook;
    cfg.amy_external_bus_postprocess_hook = clip_bounce_bus_hook;
    cfg.max_sequencer_tags = SEQ_CLIP_TAG_MAX + 2;
    cfg.max_synths = SYNTH_SLOT_COUNT;
    cfg.max_buses = FX_BUS_COUNT;
#if CONFIG_SEQ_KS_RINGS > 0
    cfg.ks_oscs = CONFIG_SEQ_KS_RINGS;
#else
    {
        uint16_t rings = sequencer_core_ks_row_demand(CONFIG_SEQ_KS_LAYERS);
#if CONFIG_SEQ_KS_RINGS_IN_PSRAM
        rings += sequencer_core_arp_voices();
#endif
        cfg.ks_oscs = (uint8_t)rings;
    }
#endif
    cfg.overload_threshold = 0.0f;
    drum_cache_init(&g_host_drums_part);     /* gamma9001_pcm_mount() */
    amy_start(cfg);
    wavetable_bank_init();
#if CONFIG_SYNTH_CUSTOM_WT
    wt_builder_init();
#endif

    project_fs_init();
    if (project_fs_ok()) project_store_cleanup_tmp();
#if CONFIG_SYNTH_PROJECT_SELFTEST
    project_store_selftest();
#endif

    s_ui_task = g_host_cur_task;
    synth_ui_init(&s_u8g2);
    amy_helpers_set_render_task(s_render_task);
    host_pump_drain();
    s_ui_next_ms = now_ms();
}

/* ── Frames ──────────────────────────────────────────────────────────── */

static frame_t current_frame(void)
{
    frame_t f = { u8g2_GetBufferPtr(&s_u8g2), u8g2_GetBufferTileWidth(&s_u8g2),
                  u8g2_GetBufferTileHeight(&s_u8g2) };
    return f;
}

#ifndef __EMSCRIPTEN__

/* ── replay: frames, WAV and projects as files ───────────────────────── */

static void write_frame(const char *dir, const char *name)
{
    char path[1024];
    frame_t f = current_frame();
    frame_look_t look = { .scale = 4, .grid = false, .band = 16 };
    snprintf(path, sizeof path, "%s/%s.txt", dir, name);
    if (frame_write_txt(path, name, &f) != 0) fprintf(stderr, "cannot write %s\n", path);
    snprintf(path, sizeof path, "%s/%s.png", dir, name);
    if (frame_write_png(path, &f, &look) != 0) fprintf(stderr, "cannot write %s\n", path);
}

/* ── WAV ─────────────────────────────────────────────────────────────── */

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4, sr = AMY_SAMPLE_RATE, br = sr * 4, v;
    uint16_t s;
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    s = 1; fwrite(&s, 2, 1, f); s = 2; fwrite(&s, 2, 1, f);
    fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); s = 4; fwrite(&s, 2, 1, f);
    s = 16; fwrite(&s, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

/* ── Projects ────────────────────────────────────────────────────────── */

static void clear_projects(void)
{
    DIR *d = opendir(PROJECT_FS_BASE);
    if (!d) return;
    struct dirent *e;
    char path[512];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", PROJECT_FS_BASE, e->d_name);
        unlink(path);
    }
    closedir(d);
}

static bool load_project_file(const char *src)
{
    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "cannot open %s\n", src); return false; }
    FILE *out = fopen(PROJECT_FS_BASE "/P00.amp", "wb");
    if (!out) { fclose(in); fprintf(stderr, "cannot write the project slot\n"); return false; }
    int c;
    while ((c = fgetc(in)) != EOF) fputc(c, out);
    fclose(in);
    fclose(out);
    if (!project_snapshot_load(0)) { fprintf(stderr, "LOAD FAILED\n"); return false; }
    host_pump_drain();
    return true;
}

/* ── replay ──────────────────────────────────────────────────────────── */

typedef struct { uint32_t block; char *text; } log_line_t;

static int replay(const char *log_path, const char *outdir, const char *project)
{
    FILE *lf = fopen(log_path, "r");
    if (!lf) { fprintf(stderr, "cannot open %s\n", log_path); return 2; }
    log_line_t *lines = NULL;
    size_t n = 0, cap = 0;
    char buf[HARNESS_LINE_MAX + 32];
    unsigned lineno = 0;
    while (fgets(buf, sizeof buf, lf)) {
        lineno++;
        buf[strcspn(buf, "\r\n")] = '\0';
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;
        unsigned blk;
        int used = 0;
        if (sscanf(p, "@%u %n", &blk, &used) != 1 || used == 0) {
            fprintf(stderr, "%s:%u: expected '@<block> <command>'\n", log_path, lineno);
            fclose(lf);
            return 2;
        }
        if (n && blk < lines[n - 1].block) {
            fprintf(stderr, "%s:%u: block numbers must not decrease\n", log_path, lineno);
            fclose(lf);
            return 2;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            lines = realloc(lines, cap * sizeof *lines);
            if (!lines) { fclose(lf); return 2; }
        }
        lines[n].block = blk;
        lines[n].text = strdup(p + used);
        n++;
    }
    fclose(lf);

    dup2(2, 1);
    if (mkdir(outdir, 0755) != 0 && errno != EEXIST) { perror(outdir); return 2; }
    if (chdir(outdir) != 0) { perror(outdir); return 2; }
    mkdir(PROJECT_FS_BASE, 0755);
    clear_projects();

    boot();
    if (project && !load_project_file(project)) return 1;
    s_block = 0;
    s_record.len = 0;

    uint32_t end = n ? lines[n - 1].block + AMY_SAMPLE_RATE / BLOCK_FRAMES : 0;
    FILE *wav = fopen("out.wav", "wb");
    if (!wav) { perror("out.wav"); return 2; }
    wav_header(wav, 0);
    int16_t block[BLOCK_FRAMES * AMY_NCHANS];
    size_t i = 0;
    bool ended = false;
    uint32_t frames = 0;
    while (!ended) {
        for (; i < n && lines[i].block == s_block; i++) {
            const char *t = lines[i].text;
            char name[128];
            int id;
            char act[8];
            long d;
            if (sscanf(t, "shot %127s", name) == 1) {
                write_frame(".", name);
            } else if (strcmp(t, "end") == 0) {
                ended = true;
            } else if (sscanf(t, "key %d %7s", &id, act) == 2) {
                key_event(id, strcmp(act, "down") == 0);
            } else if (sscanf(t, "wheel %ld", &d) == 1) {
                s_enc_accum += d;
            } else {
                run_command(t);
            }
        }
        if (ended || (i == n && s_block >= end)) break;
        step_block(block);
        fwrite(block, sizeof block, 1, wav);
        frames += BLOCK_FRAMES;
    }
    fseek(wav, 0, SEEK_SET);
    wav_header(wav, frames);
    fclose(wav);

    write_frame(".", "final");
    s_console.len = 0;
    run_command("st.seqdump");
    FILE *f = fopen("seqdump.txt", "w");
    if (f) { fwrite(s_console.buf ? s_console.buf : "", 1, s_console.len, f); fclose(f); }
    f = fopen("record.log", "w");
    if (f) { fwrite(s_record.buf ? s_record.buf : "", 1, s_record.len, f); fclose(f); }
    fprintf(stderr, "replayed %zu lines, %u blocks (%.2f s)\n", n, s_block,
            (double)frames / AMY_SAMPLE_RATE);
    return 0;
}

#endif /* !__EMSCRIPTEN__ */

/* ── Step requests: shared by serve and the WebAssembly entry ───────── */

static void json_str(sbuf_t *b, const char *s, size_t n)
{
    sb_add(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; sb_add(b, e, 2); }
        else if (c == '\n') sb_add(b, "\\n", 2);
        else if (c < 0x20 || c >= 0x7f) { snprintf(e, sizeof e, "\\u%04x", c); sb_add(b, e, 6); }
        else sb_add(b, (const char *)&c, 1);
    }
    sb_add(b, "\"", 1);
}

typedef struct { uint32_t off; char kind; int id; bool down; long delta; char *cmd; } step_ev_t;

#define STEP_MAX_EVENTS 256
static step_ev_t s_ev[STEP_MAX_EVENTS];
static size_t s_nev;

/* One input line of a step request into s_ev. Bad lines are reported on the
 * console and dropped. */
static void parse_input_line(const char *line, unsigned blocks)
{
    if (s_nev == STEP_MAX_EVENTS) return;
    step_ev_t *e = &s_ev[s_nev];
    unsigned off;
    int used = 0;
    char act[8];
    memset(e, 0, sizeof *e);
    if (sscanf(line, "key %u %d %7s", &off, &e->id, act) == 3) {
        e->kind = 'k'; e->down = strcmp(act, "down") == 0;
    } else if (sscanf(line, "enc %u %ld", &off, &e->delta) == 2) {
        e->kind = 'e';
    } else if (sscanf(line, "cmd %u %n", &off, &used) == 1 && used > 0) {
        e->kind = 'c'; e->cmd = strdup(line + used);
    } else {
        sb_printf(&s_console, "hostsim: bad input line '%s'\n", line);
        return;
    }
    e->off = off < blocks ? off : blocks - 1;
    s_nev++;
}

/* The answer to the last step request. */
static sbuf_t s_ans_json;
static int16_t *s_ans_audio;
static size_t s_ans_audio_cap, s_ans_audio_bytes, s_ans_frame_bytes;

/* Renders `blocks` blocks, applying the parsed input at its offsets, and
 * builds the answer. Consumes s_ev. */
static void step_request(unsigned blocks)
{
    size_t need = (size_t)blocks * BLOCK_FRAMES * AMY_NCHANS;
    if (need > s_ans_audio_cap) {
        int16_t *na = realloc(s_ans_audio, need * sizeof(int16_t));
        if (!na) { fprintf(stderr, "hostsim: out of memory\n"); exit(2); }
        s_ans_audio = na; s_ans_audio_cap = need;
    }
    uint32_t first_block = s_block;
    for (unsigned b = 0; b < blocks; b++) {
        for (size_t k = 0; k < s_nev; k++) {
            if (s_ev[k].off != b) continue;
            if (s_ev[k].kind == 'k') key_event(s_ev[k].id, s_ev[k].down);
            else if (s_ev[k].kind == 'e') s_enc_accum += s_ev[k].delta;
            else { run_command(s_ev[k].cmd); free(s_ev[k].cmd); s_ev[k].cmd = NULL; }
        }
        step_block(s_ans_audio + (size_t)b * BLOCK_FRAMES * AMY_NCHANS);
    }
    s_nev = 0;

    sbuf_t *js = &s_ans_json;
    js->len = 0;
    sb_printf(js, "{\"block\":%u,\"first\":%u,\"ms\":%u,\"view\":%d,"
              "\"load\":[%.1f,%.1f],\"console\":",
              s_block, first_block, now_ms(), (int)synth_ui_active_view(),
              100.0 * s_load_sum / (s_load_n ? s_load_n : 1) / LOAD_MODEL_BUDGET,
              100.0 * s_load_max / LOAD_MODEL_BUDGET);
    s_load_sum = s_load_max = 0;
    s_load_n = 0;
    json_str(js, s_console.buf ? s_console.buf : "", s_console.len);
    sb_add(js, ",\"record\":", 10);
    json_str(js, s_record.buf ? s_record.buf : "", s_record.len);
    sb_add(js, "}", 1);
    s_console.len = 0;
    s_record.len = 0;

    frame_t f = current_frame();
    s_ans_frame_bytes = s_frame_dirty ? (size_t)f.tile_w * f.tile_h * 8 : 0;
    s_ans_audio_bytes = need * sizeof(int16_t);
    s_frame_dirty = false;
}

/* Boot for a step-driven front end: projects in ./proj, block 0 from here,
 * the first answer carries a frame. */
static void session_boot(void)
{
    mkdir(PROJECT_FS_BASE, 0755);
    boot();
    s_block = 0;
    s_record.len = 0;
    s_frame_dirty = true;
}

#ifndef __EMSCRIPTEN__

/* ── serve: the step protocol on stdin / stdout ──────────────────────── */

static FILE *s_proto;

static int serve(const char *workdir)
{
    int proto_fd = dup(1);
    s_proto = fdopen(proto_fd, "wb");
    if (!s_proto) return 2;
    dup2(2, 1);                         /* stray prints go to stderr */
    if (workdir) {
        if (mkdir(workdir, 0755) != 0 && errno != EEXIST) { perror(workdir); return 2; }
        if (chdir(workdir) != 0) { perror(workdir); return 2; }
    }
    session_boot();

    char line[HARNESS_LINE_MAX + 64];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "quit") == 0) break;
        unsigned blocks;
        if (sscanf(line, "step %u", &blocks) != 1 || blocks == 0 || blocks > 4096) {
            fprintf(s_proto, "err bad request\n");
            fflush(s_proto);
            continue;
        }
        while (fgets(line, sizeof line, stdin)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strcmp(line, ".") == 0) break;
            parse_input_line(line, blocks);
        }
        step_request(blocks);
        fprintf(s_proto, "ok %zu %zu %zu\n", s_ans_json.len, s_ans_frame_bytes, s_ans_audio_bytes);
        fwrite(s_ans_json.buf, 1, s_ans_json.len, s_proto);
        if (s_ans_frame_bytes) fwrite(current_frame().buf, 1, s_ans_frame_bytes, s_proto);
        fwrite(s_ans_audio, 1, s_ans_audio_bytes, s_proto);
        fflush(s_proto);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "replay") == 0)
        return replay(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);
    if (argc >= 2 && strcmp(argv[1], "serve") == 0)
        return serve(argc >= 3 ? argv[2] : NULL);
    fprintf(stderr, "usage: hostsim replay <log> <outdir> [project.amp]\n"
                    "       hostsim serve [workdir]\n");
    return 2;
}

#else

/* ── WebAssembly entry: the step protocol as calls ───────────────────────
 * The page calls hs_boot() once, then hs_step() per stretch of audio with
 * the input lines of a step request ('\n'-separated, no terminator line);
 * the answer is read through the getters until the next hs_step(). The
 * working directory is the module filesystem's root: projects in /proj. */
#include <emscripten/emscripten.h>
#include "amy_midi.h"

/* AMY is built here as on the native host but without its audio backend
 * (AMY_NO_MINIAUDIO) or MIDI devices (AMY_HOST_MIDI); these are what that
 * backend file and the MIDI layer would have supplied. The native platform
 * init and deinit are empty too. */
void amy_platform_init(void) { }
void amy_platform_deinit(void) { }
void run_midi(void) { }
void stop_midi(void) { }
void midi_out(uint8_t *bytes, uint16_t len) { (void)bytes; (void)len; }

EMSCRIPTEN_KEEPALIVE void hs_boot(void)
{
    dup2(2, 1);                         /* stray prints go to the console log */
    session_boot();
}

/* 0 on success, -1 for a block count outside 1..4096. */
EMSCRIPTEN_KEEPALIVE int hs_step(unsigned blocks, const char *input)
{
    if (blocks == 0 || blocks > 4096) return -1;
    char line[HARNESS_LINE_MAX + 64];
    for (const char *p = input; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        if (n) parse_input_line(line, blocks);
        p = nl ? nl + 1 : NULL;
    }
    step_request(blocks);
    return 0;
}

EMSCRIPTEN_KEEPALIVE const char *hs_json(void) { return s_ans_json.buf; }
EMSCRIPTEN_KEEPALIVE int hs_json_len(void) { return (int)s_ans_json.len; }
EMSCRIPTEN_KEEPALIVE const uint8_t *hs_frame(void) { return current_frame().buf; }
EMSCRIPTEN_KEEPALIVE int hs_frame_len(void) { return (int)s_ans_frame_bytes; }
EMSCRIPTEN_KEEPALIVE const int16_t *hs_audio(void) { return s_ans_audio; }
EMSCRIPTEN_KEEPALIVE int hs_audio_len(void) { return (int)s_ans_audio_bytes; }

#endif
