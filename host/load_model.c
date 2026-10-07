/* Rough device render-load model, cycles per block at 240 MHz (budget
 * 1,280,000), behind the page's load meter. #included by hostsim.c, not
 * linked: it reads AMY's synth[] and the FX cache directly.
 *
 * Measured (2026-09-18 feature-cost bench, fixed point, LTO): idle block,
 * base osc per renderer (saw 27.6k, sine 13.2k, tri 15.7k, wavetable 24.4k,
 * KS 17.3k), one extra unison copy per renderer (saw 20.3k, sine 6.0k,
 * tri 8.6k, wavetable 16.8k), the full FX chain (~290k, split by the August
 * per-stage ratios). Guessed: PCM, noise, FM (ALGO), silent control oscs,
 * filters, copies of the unmeasured renderers. Not counted: envelopes and
 * LFOs beyond their oscs, the sequencer tick and event parsing (patch loads
 * are the device's spikes), cache and PSRAM effects. An upper-bound sanity
 * check and a relative meter, not a measurement. */
#include "amy.h"
#include "fx_bus.h"
#include "amy_fx.h"

#define LOAD_MODEL_BUDGET 1280000.0

static double load_model_block_cycles(void)
{
    double c = 31915.0;
    for (uint16_t o = 0; o < amy_global.config.max_oscs; o++) {
        struct synthinfo *sy = synth[o];
        if (sy == NULL) continue;
        if (sy->status != SYNTH_AUDIBLE) {
            continue;
        }
        double base, copy;
        switch (sy->wave) {
        case SINE: base = 13200; copy = 6000; break;
        case TRIANGLE: base = 15700; copy = 8600; break;
        case SAW_DOWN: case SAW_UP: case PULSE: base = 27600; copy = 20300; break;
        case WAVETABLE: base = 24400; copy = 16800; break;
        case KS: base = 17300; copy = 12000; break;
        case NOISE: base = 15000; copy = 8000; break;
        case PCM: case PCM_LEFT: base = 14000; copy = 8000; break;
        case ALGO: base = 6 * 13200 + 8000; copy = 6 * 6000; break;
        case SILENT: base = 6000; copy = 0; break;
        default: base = 15000; copy = 8000; break;
        }
        c += base;
        if (sy->unison_count > 1) c += copy * (sy->unison_count - 1);
        if (sy->filter_type == FILTER_LPF24) c += 12000;
        else if (sy->filter_type != FILTER_NONE) c += 7000;
    }
    for (uint8_t b = 0; b < FX_BUS_COUNT; b++) {
        if (!fx_bus_is_active(b)) continue;
        const fx_state_t *f = &s_fx[b];
        c += 9000;
        if (f->reverb_level) c += 165000;
        if (f->chorus_level) c += 63000;
        if (f->echo_level)   c += 43000;
        if (f->eq_low_db || f->eq_mid_db || f->eq_high_db) c += 15000;
        if (f->bus_dist_type) c += 20000;
    }
    return c;
}
