/* lab - the module side of the wavetable and FM Custom lab pages (page/wt,
 * page/fm, driven by page/lab.js from an instance of their own) and of the
 * device page's apply.
 *
 * The pure calls run the firmware's models (wt_synth.h, fm_voice.h) and
 * print what they give; the audio calls queue events through the app's own
 * paths (amy_helpers, fm_voice_configure_track(), wt_builder), which sound
 * in the following hs_step(). Everything here is called between steps:
 * hs_step() is synchronous, so no call lands inside a block. A returned
 * string is a static buffer, valid until the next call of the same function.
 *
 * Parameters cross as plain arrays so the page never mirrors a struct:
 * - wt: 15 bytes, shape[3], width[3], bright[3], sync[3], peak[3] (A, M, B
 *   each), plus the range as its own argument.
 * - fm: LAB_FM_N doubles (LAYOUT below). n == 0 stands for the default
 *   voice. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "amy.h"
#include "amy_helpers.h"
#include "synth_slots.h"
#include "sequencer_core.h"
#include "custompatches/fm_voice.h"
#include "custompatches/fm_graph.h"
#include "custompatches/wt_synth.h"
#include "custompatches/wt_builder.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

/* Past every app synth: synth_slots.h's contract gives slot 0 and LIVE_SYNTH to the app. */
#ifdef AMY_SYNTH_COUNT
#define LAB_SYNTH AMY_SYNTH_COUNT
#else
#define LAB_SYNTH SYNTH_SLOT_COUNT
#endif

/* ── Text answers ────────────────────────────────────────────────────── */

typedef struct { char *buf; size_t cap, len; } tbuf_t;

static void tb_printf(tbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tb_printf(tbuf_t *b, const char *fmt, ...)
{
    if (b->len + 1 >= b->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    b->len += (size_t)n < b->cap - b->len ? (size_t)n : b->cap - b->len - 1;
}

/* ── Wavetable ───────────────────────────────────────────────────────── */

_Static_assert(WT_KEYS == 3, "the wt parameters cross as three keyframes");

#define WT_VELOCITY 0.5f
#define WT_ATTACK_MS  5u    /* the note's envelope: no clicks at its edges */
#define WT_RELEASE_MS 30u

static float s_frames[WT_FRAMES * WT_CYCLE];
static uint32_t s_wt_armed;      /* wt_builder_generation() at the last arm */

static void wt_unpack(const uint8_t *v, uint8_t range, wt_params_t *p)
{
    for (int k = 0; k < WT_KEYS; k++) {
        p->shape[k]  = v[k];
        p->width[k]  = v[3 + k];
        p->bright[k] = v[6 + k];
        p->sync[k]   = v[9 + k];
        p->peak[k]   = v[12 + k];
    }
    p->range = range;
    wt_params_clamp(p);
}

static void wt_pack(const wt_params_t *p, uint8_t *v)
{
    for (int k = 0; k < WT_KEYS; k++) {
        v[k]      = p->shape[k];
        v[3 + k]  = p->width[k];
        v[6 + k]  = p->bright[k];
        v[9 + k]  = p->sync[k];
        v[12 + k] = p->peak[k];
    }
}

/* Build the whole table into out[WT_FRAMES * WT_CYCLE]; v is clamped in
 * place. Returns the whole-table scale wt_synth_finish() applied (1.0 when
 * the peak fits). */
EMSCRIPTEN_KEEPALIVE float lab_wt_table(uint8_t *v, uint8_t range, int16_t *out)
{
    wt_params_t p;
    wt_preview_t pv;
    wt_unpack(v, range, &p);
    wt_pack(&p, v);
    wt_synth_build_frames(&p, 0, WT_FRAMES, s_frames);
    float peak = 0.0f;
    for (int i = 0; i < WT_FRAMES * WT_CYCLE; i++) {
        float a = fabsf(s_frames[i]);
        if (a > peak) peak = a;
    }
    wt_synth_finish(s_frames, out, &pv);
    return peak > 32000.0f ? 32000.0f / peak : 1.0f;
}

/* M to the halfway blend of A and B, in place. */
EMSCRIPTEN_KEEPALIVE void lab_wt_blend(uint8_t *v)
{
    wt_params_t p;
    wt_unpack(v, 0, &p);
    wt_params_blend_mid(&p);
    wt_pack(&p, v);
}

EMSCRIPTEN_KEEPALIVE const char *lab_wt_meta(uint8_t range)
{
    static char text[160];
    snprintf(text, sizeof text,
             "{\"harmonics\":%u,\"clean_note\":%u,\"frames\":%d,\"cycle\":%d,\"mid_frame\":%d}",
             wt_synth_harmonics(range), wt_synth_clean_note(range), WT_FRAMES, WT_CYCLE,
             WT_MID_FRAME);
    return text;
}

/* The builder rebuilds the live table over the UI slices of the next
 * steps; lab_wt_ready() says when that rebuild has finished. */
EMSCRIPTEN_KEEPALIVE void lab_wt_arm(const uint8_t *v, uint8_t range)
{
    wt_params_t p;
    wt_unpack(v, range, &p);
    s_wt_armed = wt_builder_generation();
    wt_builder_set_params(&p);
}

EMSCRIPTEN_KEEPALIVE int lab_wt_ready(void)
{
    return wt_builder_generation() != s_wt_armed;
}

/* A note-on sets LAB_SYNTH up as one voice of one WAVETABLE osc on the live
 * table, at position pos (0..1); on == 0 releases the note. */
EMSCRIPTEN_KEEPALIVE void lab_wt_note(int midi, float pos, int on)
{
    amy_event *e;
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;
    if (on) {
        e = amy_helpers_event_begin();
        e->synth          = LAB_SYNTH;
        e->num_voices     = 1;
        e->oscs_per_voice = 1;
        amy_helpers_event_send(e);

        e = amy_helpers_event_begin();
        e->synth = LAB_SYNTH;
        e->osc   = 0;
        e->wave  = WAVETABLE;
        if (wt_builder_preset() != WT_BUILDER_PRESET_NONE) e->preset = (int16_t)wt_builder_preset();
        e->bp_is_set[0]  = 1;
        e->eg0_times[0]  = WT_ATTACK_MS;  e->eg0_values[0] = 1.0f;
        e->eg0_times[1]  = WT_RELEASE_MS; e->eg0_values[1] = 0.0f;
        e->duty_coefs[COEF_CONST] = pos < 0.0f ? 0.0f : pos > 1.0f ? 1.0f : pos;
        amy_helpers_event_send(e);
    }
    e = amy_helpers_event_begin();
    e->synth     = LAB_SYNTH;
    e->midi_note = (float)midi;
    e->velocity  = on ? WT_VELOCITY : 0.0f;
    amy_helpers_event_send(e);
}

/* The sweep: the position (0..1) of the sounding note only. */
EMSCRIPTEN_KEEPALIVE void lab_wt_pos(float pos)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth = LAB_SYNTH;
    e->osc   = 0;
    e->duty_coefs[COEF_CONST] = pos < 0.0f ? 0.0f : pos > 1.0f ? 1.0f : pos;
    amy_helpers_event_send(e);
}

/* The device page's apply: the app's custom table takes these parameters
 * (clamped); the UI slices rebuild it. */
EMSCRIPTEN_KEEPALIVE void lab_wt_apply(const uint8_t *v, uint8_t range)
{
    wt_params_t p;
    wt_unpack(v, range, &p);
    wt_builder_set_params(&p);
}

/* ── FM voice ────────────────────────────────────────────────────────── */

/* LAYOUT of the voice as numbers: algorithm, fb_op, feedback, op_fixed,
 * op_mute, then per operator: targets, ratio, fixed_hz, level, R1..R4,
 * L1..L4, eg_type. Only this file reads it; the page passes it back opaque. */
#define FM_HEAD    5
#define FM_PER_OP  13
#define LAB_FM_N   (FM_HEAD + FM_PER_OP * FM_NUM_OPS)

_Static_assert(FM_FIELD_COARSE == 0 && FM_FIELD_R1 == 3 && FM_FIELD_L1 == 7 && FM_FIELD_FEEDBACK == 11,
               "the page's field numbers follow fm_field_t");

static int s_fm_base = -1;       /* LAB_SYNTH's control osc once found, -1 before */

static uint32_t to_u(double x, uint32_t max)
{
    if (!(x >= 0.0)) return 0;
    return x > (double)max ? max : (uint32_t)x;
}

static void voice_from(const double *n, fm_voice_t *v)
{
    memset(v, 0, sizeof(*v));
    v->algorithm = (uint8_t)to_u(n[0], 255);
    if (v->algorithm == 0u) v->algorithm = 1;       /* row 0 aliases row 1 (fm_voice.h) */
    v->fb_op    = (uint8_t)to_u(n[1], 255);
    v->feedback = (float)n[2];
    v->op_fixed = (uint8_t)to_u(n[3], 255);
    v->op_mute  = (uint8_t)to_u(n[4], 255);
    for (int i = 0; i < FM_NUM_OPS; i++) {
        const double *o = n + FM_HEAD + FM_PER_OP * i;
        v->op_targets[i]  = (uint8_t)to_u(o[0], 255);
        v->op_ratio[i]    = (float)o[1];
        v->op_fixed_hz[i] = (float)o[2];
        v->op_level[i]    = (float)o[3];
        for (int s = 0; s < 4; s++) {
            v->op_env[i].rate[s]  = (uint8_t)to_u(o[4 + s], 255);
            v->op_env[i].level[s] = (uint8_t)to_u(o[8 + s], 255);
        }
        v->op_env[i].eg_type = (uint8_t)to_u(o[12], 255);
    }
    fm_voice_clamp(v);
}

/* false when n is neither 0 (the default voice) nor LAB_FM_N. */
static bool voice_read(const double *n, int count, fm_voice_t *v)
{
    if (count == 0) { fm_voice_default(v); return true; }
    if (count != LAB_FM_N || !n) return false;
    voice_from(n, v);
    return true;
}

static void print_voice(tbuf_t *b, const fm_voice_t *v, const char *err)
{
    fm_graph_view_t g;
    fm_program_t p;
    fm_slot_trace_t tr[FM_GRAPH_OPS];
    bool custom = v->algorithm == FM_ALGO_CUSTOM;
    int index = custom ? (int)amy_num_algorithms : (int)v->algorithm;
    fm_voice_graph(v, &g);
    if (custom) {
        fm_graph_compile(v->op_targets, v->fb_op, &p);      /* clamped voices always compile */
    } else {
        const uint8_t *ops = amy_algorithm_ops(v->algorithm);
        for (int s = 0; s < FM_NUM_OPS; s++) { p.ops[s] = ops ? ops[s] : 0; p.slot_op[s] = (uint8_t)s; }
    }
    fm_graph_trace(&p, tr);

    tb_printf(b, "{\"v\":[%u,%u,%.9g,%u,%u", v->algorithm, v->fb_op, (double)v->feedback, v->op_fixed,
              v->op_mute);
    for (int i = 0; i < FM_NUM_OPS; i++) {
        const fm_op_env_t *e = &v->op_env[i];
        tb_printf(b, ",%u,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%u,%u,%u", v->op_targets[i],
                  (double)v->op_ratio[i], (double)v->op_fixed_hz[i], (double)v->op_level[i],
                  e->rate[0], e->rate[1], e->rate[2], e->rate[3],
                  e->level[0], e->level[1], e->level[2], e->level[3], e->eg_type);
    }
    tb_printf(b, "],\"err\":%s%s%s,\"algorithm\":%u,\"custom\":%s,\"feedback\":%.9g,\"fb_op\":",
              err ? "\"" : "", err ? err : "null", err ? "\"" : "", custom ? 0u : v->algorithm,
              custom ? "true" : "false", (double)v->feedback);
    if (g.fb_op == FM_OP_NONE) tb_printf(b, "null"); else tb_printf(b, "%u", g.fb_op);
    tb_printf(b, ",\"out_bit\":%u,\"ops\":[", FM_OUT_BIT);
    for (int i = 0; i < FM_NUM_OPS; i++) {
        const fm_op_env_t *e = &v->op_env[i];
        bool fixed = (v->op_fixed & (1u << i)) != 0u;
        uint32_t t_ms[4];
        fm_voice_env_times_ms(e, t_ms);
        tb_printf(b, "%s{\"label\":\"OP%d\",\"fixed\":%s,\"ratio\":%.9g,\"fixed_hz\":%.9g,\"a4_hz\":%.9g,"
                  "\"level\":%.9g,\"mute\":%s,\"r\":[%u,%u,%u,%u],\"t\":[%u,%u,%u,%u],\"l\":[%u,%u,%u,%u],"
                  "\"targets\":%u}",
                  i ? "," : "", FM_NUM_OPS - i, fixed ? "true" : "false", (double)v->op_ratio[i],
                  (double)v->op_fixed_hz[i], (double)(fixed ? v->op_fixed_hz[i] : FM_A4_HZ * v->op_ratio[i]),
                  (double)v->op_level[i], (v->op_mute & (1u << i)) ? "true" : "false",
                  e->rate[0], e->rate[1], e->rate[2], e->rate[3],
                  (unsigned)t_ms[0], (unsigned)t_ms[1], (unsigned)t_ms[2], (unsigned)t_ms[3],
                  e->level[0], e->level[1], e->level[2], e->level[3], g.out_mask[i]);
    }
    tb_printf(b, "],\"program\":{\"index\":%d,\"slots\":[", index);
    for (int s = 0; s < FM_GRAPH_OPS; s++) {
        tb_printf(b, "%s{\"op\":%u,\"byte\":%u,\"in\":%u,\"out\":%u,\"add\":%s,\"fb\":%s,\"bus1\":%u,\"bus2\":%u}",
                  s ? "," : "", p.slot_op[s], p.ops[s], tr[s].in_bus, tr[s].out_bus,
                  tr[s].add ? "true" : "false", tr[s].fb ? "true" : "false", tr[s].bus1, tr[s].bus2);
    }
    tb_printf(b, "]}}\n");
}

/* An error answer; quotes, backslashes and control characters in the
 * message become '?'. */
static const char *voice_err(tbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static const char *voice_err(tbuf_t *b, const char *fmt, ...)
{
    char msg[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    for (char *c = msg; *c; c++) {
        if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) *c = '?';
    }
    b->len = 0;
    tb_printf(b, "{\"err\":\"%s\"}", msg);
    return b->buf;
}

/* One edit of the voice through fm_voice.h's setters and step grid, or
 * none ("describe"); the answer is the voice after it. cmd: describe,
 * default, step (a0 op, a1 field, a2 delta), fixed (a0 op), mute (a0 op),
 * algo (a0 dir), targets (a0 op, a1 mask), fb (a0 op, 255 = none); unused
 * arguments are ignored. */
EMSCRIPTEN_KEEPALIVE const char *lab_fm_voice(const double *n, int count, const char *cmd,
                                              int a0, int a1, int a2)
{
    static const char *const route_err[] = { NULL, "INVALID", "LOOP", "NO_BUS" };
    static char text[8192];
    tbuf_t b = { text, sizeof text, 0 };
    const char *err = NULL;
    fm_voice_t v;
    if (!voice_read(n, count, &v)) return voice_err(&b, "v needs %d values", LAB_FM_N);
    if (!cmd || !*cmd) cmd = "describe";

    if (!strcmp(cmd, "describe")) {
    } else if (!strcmp(cmd, "default")) {
        fm_voice_default(&v);
    } else if (!strcmp(cmd, "step")) {
        if (a1 < 0 || a1 >= FM_FIELD_COUNT) return voice_err(&b, "step: field %d", a1);
        fm_voice_step(&v, (uint8_t)to_u(a0, 255), (fm_field_t)a1, a2);
    } else if (!strcmp(cmd, "fixed")) {
        fm_voice_toggle_fixed(&v, (uint8_t)to_u(a0, 255));
    } else if (!strcmp(cmd, "mute")) {
        uint32_t op = to_u(a0, 255);
        if (op < FM_NUM_OPS) v.op_mute = (uint8_t)(v.op_mute ^ (1u << op));
    } else if (!strcmp(cmd, "algo")) {
        fm_voice_step_algorithm(&v, a0);
    } else if (!strcmp(cmd, "targets")) {
        fm_route_err_t e;
        fm_voice_set_op_targets(&v, (uint8_t)to_u(a0, 255), (uint8_t)to_u(a1, 255), &e);
        err = route_err[e];
    } else if (!strcmp(cmd, "fb")) {
        if (!fm_voice_set_fb_op(&v, (uint8_t)to_u(a0, 255))) err = "INVALID";
    } else {
        return voice_err(&b, "unknown command '%.32s'", cmd);
    }
    print_voice(&b, &v, err);
    return text;
}

/* A float as a C literal: "1.0f", never "1f". */
static void print_f(tbuf_t *b, float x)
{
    char s[32];
    snprintf(s, sizeof s, "%.9g", (double)x);
    tb_printf(b, "%s%sf", s, strpbrk(s, ".e") ? "" : ".0");
}

/* The voice as an fm_voice_t designated initializer. */
EMSCRIPTEN_KEEPALIVE const char *lab_fm_init(const double *n, int count)
{
    static const char *const eg[] = { "ENVELOPE_NORMAL", "ENVELOPE_LINEAR", "ENVELOPE_DX7",
                                      "ENVELOPE_TRUE_EXPONENTIAL" };
    static char text[4096];
    tbuf_t b = { text, sizeof text, 0 };
    fm_voice_t v;
    if (!voice_read(n, count, &v)) return voice_err(&b, "v needs %d values", LAB_FM_N);
    tb_printf(&b, "static const fm_voice_t s_voice = {\n");
    if (v.algorithm == FM_ALGO_CUSTOM) tb_printf(&b, "    .algorithm   = FM_ALGO_CUSTOM,\n");
    else                               tb_printf(&b, "    .algorithm   = %u,\n", v.algorithm);
    if (v.fb_op == FM_OP_NONE) tb_printf(&b, "    .fb_op       = FM_OP_NONE,\n");
    else                       tb_printf(&b, "    .fb_op       = %u,\n", v.fb_op);
    tb_printf(&b, "    .op_targets  = {");
    for (int i = 0; i < FM_NUM_OPS; i++)
        tb_printf(&b, " 0x%02X%s", v.op_targets[i], i < FM_NUM_OPS - 1 ? "," : " },\n");
    const float *rows[] = { v.op_ratio, v.op_fixed_hz, v.op_level };
    const char *names[] = { ".op_ratio    ", ".op_fixed_hz ", ".op_level    " };
    for (int r = 0; r < 3; r++) {
        tb_printf(&b, "    %s= {", names[r]);
        for (int i = 0; i < FM_NUM_OPS; i++) {
            tb_printf(&b, " ");
            print_f(&b, rows[r][i]);
            tb_printf(&b, "%s", i < FM_NUM_OPS - 1 ? "," : " },\n");
        }
    }
    tb_printf(&b, "    .op_env      = {\n");
    for (int i = 0; i < FM_NUM_OPS; i++) {
        const fm_op_env_t *e = &v.op_env[i];
        tb_printf(&b, "        { .rate = { %u, %u, %u, %u }, .level = { %u, %u, %u, %u }, .eg_type = %s },   /* OP%d */\n",
                  e->rate[0], e->rate[1], e->rate[2], e->rate[3],
                  e->level[0], e->level[1], e->level[2], e->level[3], eg[e->eg_type & 3u], FM_NUM_OPS - i);
    }
    tb_printf(&b, "    },\n    .feedback    = ");
    print_f(&b, v.feedback);
    tb_printf(&b, ",\n    .op_fixed    = 0x%02X,\n};\n", v.op_fixed);
    return text;
}

/* LAB_SYNTH as one voice of the given voice, as a melodic row sets up
 * SEQ_PATCH_FM_CUSTOM. adsr: NULL, or a row's {A ms, D ms, S %, R ms} on the
 * control osc, the VCA over the carriers, through the rows' own push. */
EMSCRIPTEN_KEEPALIVE void lab_fm_arm(const double *n, int count, const int *adsr)
{
    fm_voice_t v;
    if (!voice_read(n, count, &v)) return;
    fm_voice_configure_track(LAB_SYNTH, 1, &v);
    if (adsr) {
        seq_env_t env = {
            .attack_ms   = adsr[0] < 0 ? 0u : (uint32_t)adsr[0],
            .decay_ms    = adsr[1] < 0 ? 0u : (uint32_t)adsr[1],
            .sustain_pct = (uint8_t)(adsr[2] < 0 ? 0 : adsr[2] > 100 ? 100 : adsr[2]),
            .release_ms  = adsr[3] < 0 ? 0u : (uint32_t)adsr[3],
            .eg_type     = ENVELOPE_NORMAL,
        };
        sequencer_core_push_envelope_osc(LAB_SYNTH, sequencer_core_patch_voice_osc(SEQ_PATCH_FM_CUSTOM), &env);
    }
    s_fm_base = -1;
}

/* Note on (velocity 1) or off on LAB_SYNTH. */
EMSCRIPTEN_KEEPALIVE void lab_fm_note(int midi, int on)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth     = LAB_SYNTH;
    e->midi_note = (float)(midi < 0 ? 0 : midi > 127 ? 127 : midi);
    e->velocity  = on ? 1.0f : 0.0f;
    amy_helpers_event_send(e);
}

/* After a step: the envelope values AMY computed for the block, msynth[]->amp
 * of the control osc (out[0], velocity x the row ADSR) and of the six
 * operators (out[1..6], level x their envelope, 0 when muted). Once the
 * control osc has finished its release AMY stops rendering the voice and
 * these go stale: out is all 0 and the answer 0 from there, and before the
 * voice exists. */
EMSCRIPTEN_KEEPALIVE int lab_fm_trace_sample(float out[FM_NUM_OPS + 1])
{
    memset(out, 0, (FM_NUM_OPS + 1) * sizeof(float));
    if (s_fm_base < 0) {
        uint16_t voices[MAX_VOICES_PER_INSTRUMENT], base;
        if (instrument_get_num_voices(LAB_SYNTH, voices) > 0 && amy_voice_base_osc(voices[0], &base) &&
            base + FM_NUM_OPS < amy_global.config.max_oscs && synth[base] && synth[base]->wave == ALGO)
            s_fm_base = base;
    }
    if (s_fm_base < 0 || !synth[s_fm_base] || synth[s_fm_base]->status == SYNTH_OFF) return 0;
    for (int k = 0; k <= FM_NUM_OPS; k++) {
        if (msynth[s_fm_base + k]) out[k] = msynth[s_fm_base + k]->amp;
    }
    return 1;
}

/* The device page's apply: the voice becomes the app's custom FM voice and
 * goes out to every row and the arp playing it, as an edit on the FM screen
 * does. */
EMSCRIPTEN_KEEPALIVE void lab_fm_apply(const double *n, int count)
{
    fm_voice_t v;
    if (!voice_read(n, count, &v)) return;
    s_fm_voice = v;
    sequencer_core_fm_voice_changed(FM_PUSH_ALL);
}
