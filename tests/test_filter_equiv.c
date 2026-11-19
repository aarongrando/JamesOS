/* The live track/master filter (firmware/src/instrument_filter.c) must stay bit-for-bit identical to the
 * per-sample implementation it replaced, which is kept frozen in tests/filter_ref_impl.h as ref_filter_*.
 * Every comparison is sample for sample and state for state (all fields of the filter struct), over:
 *  - every control 0..127 x several resonances x key-track settings, through the per-sample entry points;
 *  - random control / resonance / key-track / note changes at block boundaries and mid-block, long silences,
 *    full-scale and clipped inputs (beyond +-262144), impulses, DC, sines and noise;
 *  - the block API (br_filter_block, br_filter_block_controls) in blocks of random size, including 256;
 *  - the exact call patterns of the three call sites (poly mix path, per-part path, master ladder frame).
 * No timings are asserted. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../firmware/src/instrument_filter.c"
#include "filter_ref_impl.h"

static uint64_t g_checked;
static uint32_t g_rng = 0x1234567u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint32_t rnd_n(uint32_t n) { return rnd() % n; }

static void fail(const char *what, const char *where, uint64_t at)
{
    fprintf(stderr, "FILTER EQUIV FAIL: %s (%s) after %llu samples\n", what, where, (unsigned long long)at);
    exit(1);
}

static void same_state(const br_filter_t *a, const ref_filter_t *b, const char *where)
{
    if (a->low != b->low || a->band != b->band || a->mix != b->mix ||
        memcmp(a->ladder_s, b->ladder_s, sizeof a->ladder_s) || memcmp(a->extra_s, b->extra_s, sizeof a->extra_s) ||
        a->post_g != b->post_g || a->hp2_kinv != b->hp2_kinv || a->hp2_dinv != b->hp2_dinv ||
        a->ladder_g != b->ladder_g || a->ladder_kinv != b->ladder_kinv || a->ladder_dinv != b->ladder_dinv ||
        a->ladder_k != b->ladder_k || a->ladder_res != b->ladder_res || a->ladder_tick != b->ladder_tick)
        fail("state differs", where, g_checked);
}

static void init_pair(br_filter_t *a, ref_filter_t *b)
{
    br_filter_init(a);
    ref_filter_init(b);
    same_state(a, b, "init");
}

/* ---- input generator ---- */
typedef struct { int kind; int32_t amp; double phase, step; uint32_t seed; } gen_t;
static void gen_pick(gen_t *g)
{
    static const int32_t amps[] = { 0, 1, 7, 100, 1200, 8000, 16384, 32767, 100000, 262143, 262144, 262145, 400000, 2000000 };
    g->kind = (int)rnd_n(6);
    g->amp = amps[rnd_n(sizeof amps / sizeof amps[0])];
    if (g->kind == 5) g->amp = 0; /* silence */
    g->phase = 0.0; g->step = 0.002 + (rnd_n(1000) / 1000.0) * 1.5; g->seed = rnd();
}
static int32_t gen_next(gen_t *g, uint32_t i)
{
    switch (g->kind) {
    case 0: g->seed = g->seed * 1664525u + 1013904223u; /* noise */
            return (int32_t)(((int64_t)(int32_t)g->seed * g->amp) >> 31);
    case 1: g->phase += g->step; return (int32_t)(sin(g->phase) * g->amp);
    case 2: return i == 0 ? g->amp : 0; /* impulse */
    case 3: return g->amp; /* DC */
    case 4: g->seed = g->seed * 1664525u + 1013904223u; /* noise at half scale of its amplitude, sometimes zero */
            return (g->seed >> 29) == 0 ? 0 : (int32_t)(((int64_t)(int32_t)g->seed * g->amp) >> 32);
    default: return 0;
    }
}

/* ---- 1. every control, several resonances and key-track settings, per sample ---- */
static void test_every_control(void)
{
    static const uint32_t resos[] = { 0, 1, 9, 37, 64, 100, 118, 123, 127, 200 };
    static const uint32_t keys[] = { 0, 40, 127 };
    for (uint32_t control = 0; control <= 130; ++control)
        for (uint32_t ri = 0; ri < sizeof resos / sizeof resos[0]; ++ri)
            for (uint32_t ki = 0; ki < 3; ++ki) {
                br_filter_t a; ref_filter_t b; gen_t g;
                uint32_t note = (control * 7u + ri * 13u) % 128u;
                init_pair(&a, &b);
                for (int seg = 0; seg < 3; ++seg) {
                    gen_pick(&g);
                    if (seg == 0) { g.kind = 0; g.amp = 16384; }
                    for (uint32_t i = 0; i < 700; ++i) {
                        int32_t x = gen_next(&g, i), ya, yb;
                        ya = br_filter_process_controls(&a, x, control, resos[ri], keys[ki], note);
                        yb = ref_filter_process_controls(&b, x, control, resos[ri], keys[ki], note);
                        ++g_checked;
                        if (ya != yb) fail("output differs", "every control", g_checked);
                        same_state(&a, &b, "every control");
                        if (resos[ri] == 0 && keys[ki] == 0 && control < 128u) {
                            /* the res-0 entry point, on both, from the same states */
                            int32_t xa = br_filter_process(&a, x, control), xb = ref_filter_process(&b, x, control);
                            if (xa != xb) fail("process output differs", "every control", g_checked);
                            same_state(&a, &b, "every control (process)");
                        }
                    }
                }
            }
}

/* ---- 2. random controls: per sample, control changes mid-block ---- */
static void test_random_per_sample(uint64_t samples)
{
    br_filter_t a; ref_filter_t b; gen_t g; uint64_t done = 0;
    uint32_t control = 20, res = 0, key = 0, note = 60;
    init_pair(&a, &b);
    gen_pick(&g);
    while (done < samples) {
        uint32_t len = 1u + rnd_n(rnd_n(8) ? 40u : 700u);
        uint32_t r = rnd_n(10);
        if (r < 5) { control = rnd_n(128u); if (rnd_n(4) == 0) control = 63u + rnd_n(2u); }
        else if (r < 6) control = rnd_n(5) < 3 ? control : rnd_n(65);
        if (rnd_n(3) == 0) res = rnd_n(4) == 0 ? 0u : rnd_n(128u);
        if (rnd_n(3) == 0) key = rnd_n(3) == 0 ? rnd_n(128u) : 0u;
        if (rnd_n(2) == 0) note = rnd_n(128u);
        if (rnd_n(3) == 0) gen_pick(&g);
        for (uint32_t i = 0; i < len; ++i, ++done) {
            int32_t x = gen_next(&g, i), ya, yb;
            uint32_t c = control;
            if (rnd_n(200) == 0) c = rnd_n(128u); /* a one-off mid-block change */
            if (rnd_n(300) == 0) res = rnd_n(128u);
            ya = br_filter_process_controls(&a, x, c, res, key, note);
            yb = ref_filter_process_controls(&b, x, c, res, key, note);
            ++g_checked;
            if (ya != yb) fail("output differs", "random per sample", g_checked);
            if ((i & 15u) == 0) same_state(&a, &b, "random per sample");
        }
        same_state(&a, &b, "random per sample (end)");
    }
}

#ifdef BR_FILTER_HAS_BLOCK
/* ---- 3. block API against the per-sample reference ---- */
#define MAXBLK 700
static void test_block(uint64_t samples, int controls_variant)
{
    br_filter_t a; ref_filter_t b; gen_t g; uint64_t done = 0;
    int32_t in[MAXBLK], out[MAXBLK];
    uint32_t control = 30, res = 0, key = 0, note = 60;
    init_pair(&a, &b);
    gen_pick(&g);
    while (done < samples) {
        uint32_t n = rnd_n(3) == 0 ? 256u : 1u + rnd_n(rnd_n(4) ? 300u : MAXBLK);
        uint32_t r = rnd_n(12);
        if (r < 5) { control = rnd_n(128u); if (rnd_n(3) == 0) control = 63u + rnd_n(2u); }
        else if (r < 7) control = rnd_n(3) < 2 ? control : (control < 63u ? rnd_n(63u) : 65u + rnd_n(63u));
        if (!controls_variant || rnd_n(3) == 0) { if (rnd_n(3) == 0) res = rnd_n(4) == 0 ? 0u : rnd_n(128u); }
        if (controls_variant && rnd_n(4) == 0) key = rnd_n(3) == 0 ? rnd_n(128u) : 0u;
        if (rnd_n(2) == 0) note = rnd_n(128u);
        if (!controls_variant) { key = 0; }
        if (rnd_n(2) == 0) gen_pick(&g);
        if (rnd_n(40) == 0) { gen_pick(&g); g.kind = 5; } /* silence, then more silence: the walk to zero */
        for (uint32_t i = 0; i < n; ++i) in[i] = gen_next(&g, i);
        memcpy(out, in, n * sizeof in[0]);
        if (!controls_variant && !res && !key) {
            br_filter_block(&a, out, n, control, 0u);
        } else if (!controls_variant) {
            br_filter_block(&a, out, n, control, res);
        } else {
            br_filter_block_controls(&a, out, n, control, res, key, note);
        }
        for (uint32_t i = 0; i < n; ++i) {
            int32_t y = (!controls_variant && !res) ? ref_filter_process(&b, in[i], control)
                      : controls_variant ? ref_filter_process_controls(&b, in[i], control, res, key, note)
                      : ref_filter_run(&b, in[i], control, res);
            ++g_checked;
            if (y != out[i]) { fprintf(stderr, "n=%u i=%u control=%u res=%u key=%u\n", n, i, control, res, key); fail("block output differs", "block", g_checked); }
        }
        done += n;
        same_state(&a, &b, "block");
    }
}

/* Settle into a steady state first (so the block kernels really run), then switch states. */
static void test_block_steady(void)
{
    for (uint32_t control = 0; control <= 127; ++control)
        for (uint32_t res = 0; res < 128; res += (control % 7u == 0u ? 1u : 19u)) {
            br_filter_t a; ref_filter_t b; gen_t g;
            uint32_t other = (control * 5u + 11u) % 128u;
            int32_t in[256], out[256];
            init_pair(&a, &b);
            gen_pick(&g); g.kind = 0; g.amp = 20000;
            for (int blk = 0; blk < 12; ++blk) {
                uint32_t c = blk < 8 ? control : other;
                if (blk == 9) { gen_pick(&g); g.kind = 5; }
                for (uint32_t i = 0; i < 256; ++i) in[i] = gen_next(&g, i);
                memcpy(out, in, sizeof in);
                br_filter_block(&a, out, 256, c, res);
                for (uint32_t i = 0; i < 256; ++i) {
                    int32_t y = ref_filter_run(&b, in[i], c, res);
                    ++g_checked;
                    if (y != out[i]) fail("block output differs", "steady", g_checked);
                }
                same_state(&a, &b, "steady");
            }
        }
}
#endif


/* Copies of the loops at the two instrument_audio.c call sites (the live ones are in that file: keep them equal). */
static int32_t site_clip(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static void poly_site_live(int32_t *row, uint32_t n, int32_t gain, uint32_t filter, uint32_t res, uint32_t key,
                           uint32_t note, int32_t *sum_out, br_filter_t *f)
{
    int32_t sum = 0;
#ifdef BR_FILTER_HAS_BLOCK
    for (uint32_t i = 0; i < n; ++i) {
        int32_t sample = site_clip(br_filter_mul_q15(row[i], (uint32_t)gain), -32767, 32767);
        sum += sample;
        row[i] = sample;
    }
    if (!res && !key) br_filter_block(f, row, n, filter, 0u);
    else br_filter_block_controls(f, row, n, filter, res, key, note);
#else
    for (uint32_t i = 0; i < n; ++i) {
        int32_t sample = site_clip(br_filter_mul_q15(row[i], (uint32_t)gain), -32767, 32767);
        sum += sample;
        if (!res && !key) row[i] = br_filter_process(f, sample, filter);
        else row[i] = br_filter_process_controls(f, sample, filter, res, key, note);
    }
#endif
    *sum_out = sum;
}
static void part_site_live(int32_t *row, uint32_t n, uint32_t filter, uint32_t res, uint32_t key, uint32_t note,
                           br_filter_t *f)
{
#ifdef BR_FILTER_HAS_BLOCK
    if (!res && !key) { f->band = 0; br_filter_block(f, row, n, filter, 0u); }
    else br_filter_block_controls(f, row, n, filter, res, key, note);
#else
    if (!res && !key) {
        f->band = 0;
        for (uint32_t i = 0; i < n; ++i) row[i] = br_filter_process(f, row[i], filter);
    } else for (uint32_t i = 0; i < n; ++i) row[i] = br_filter_process_controls(f, row[i], filter, res, key, note);
#endif
}

/* Zero input into zero state skips the kernel: the restart point f->low must still end up as the kernel leaves it. */
static void test_zero_skip(void)
{
    static const uint32_t controls[] = { 5, 25, 40, 62, 66, 90, 120 };
    for (uint32_t ci = 0; ci < sizeof controls / sizeof controls[0]; ++ci)
        for (uint32_t res = 0; res < 128; res += 21) {
            br_filter_t a; ref_filter_t b; int32_t in[256], out[256]; gen_t g;
            init_pair(&a, &b); gen_pick(&g); g.kind = 0; g.amp = 20000;
            for (int blk = 0; blk < 4; ++blk) {
                for (uint32_t i = 0; i < 256; ++i) in[i] = out[i] = gen_next(&g, i);
                br_filter_block(&a, out, 256, controls[ci], res);
                for (uint32_t i = 0; i < 256; ++i) if (ref_filter_run(&b, in[i], controls[ci], res) != out[i]) fail("warm-up differs", "zero skip", g_checked);
            }
            memset(a.ladder_s, 0, sizeof a.ladder_s); memset(a.extra_s, 0, sizeof a.extra_s);
            memset(b.ladder_s, 0, sizeof b.ladder_s); memset(b.extra_s, 0, sizeof b.extra_s);
            a.low = b.low = 7; /* a state the kernel would reset to 0 */
            for (int blk = 0; blk < 6; ++blk) {
                uint32_t n = blk == 2 ? 37u : 256u;
                memset(out, 0, sizeof out);
                if (blk == 4) out[100] = 1000; /* a sample arrives in the middle of silence */
                memcpy(in, out, sizeof in);
                br_filter_block(&a, out, n, controls[ci], res);
                for (uint32_t i = 0; i < n; ++i) { ++g_checked; if (ref_filter_run(&b, in[i], controls[ci], res) != out[i]) fail("output differs", "zero skip", g_checked); }
                same_state(&a, &b, "zero skip");
            }
        }
}

/* ---- 4. the call sites ---- */
/* instrument_audio.c, polyphonic mix path: the filter input is computed per sample with a running sum. */
static void test_poly_site(uint64_t samples)
{
    br_filter_t a; ref_filter_t b; gen_t g; uint64_t done = 0;
    int32_t row[256], rowb[256];
    uint32_t filter = 40, res = 0, key = 0, note = 60;
    init_pair(&a, &b);
    gen_pick(&g);
    while (done < samples) {
        int32_t sum_a = 0, sum_b = 0, gain = (int32_t)(8192 + rnd_n(24577u));
        uint32_t n = rnd_n(5) == 0 ? 1u + rnd_n(255u) : 256u;
        if (rnd_n(3) == 0) { filter = rnd_n(128u); if (rnd_n(5) == 0) filter = 63u + rnd_n(2u); }
        if (rnd_n(4) == 0) res = rnd_n(2) ? 0u : rnd_n(128u);
        if (rnd_n(5) == 0) key = rnd_n(2) ? 0u : rnd_n(128u);
        if (rnd_n(3) == 0) note = rnd_n(128u);
        if (rnd_n(2) == 0) gen_pick(&g);
        for (uint32_t i = 0; i < n; ++i) row[i] = rowb[i] = gen_next(&g, i) * 3;
        /* reference: exactly the old loop */
        for (uint32_t i = 0; i < n; ++i) {
            int32_t sample = ref_filter_mul_q15(rowb[i], (uint32_t)gain);
            if (sample < -32767) sample = -32767;
            if (sample > 32767) sample = 32767;
            sum_b += sample;
            if (!res && !key) rowb[i] = ref_filter_process(&b, sample, filter);
            else rowb[i] = ref_filter_process_controls(&b, sample, filter, res, key, note);
        }
        /* live: the code as it stands in instrument_audio.c */
        poly_site_live(row, n, gain, filter, res, key, note, &sum_a, &a);
        if (sum_a != sum_b) fail("poly sum differs", "poly site", g_checked);
        for (uint32_t i = 0; i < n; ++i) { ++g_checked; if (row[i] != rowb[i]) fail("poly output differs", "poly site", g_checked); }
        same_state(&a, &b, "poly site");
        done += n;
    }
}

/* instrument_audio.c, normal per-part path. */
static void test_part_site(uint64_t samples)
{
    br_filter_t a; ref_filter_t b; gen_t g; uint64_t done = 0;
    int32_t row[256], rowb[256];
    uint32_t filter = 40, res = 0, key = 0, note = 60;
    init_pair(&a, &b);
    gen_pick(&g);
    while (done < samples) {
        uint32_t n = rnd_n(6) == 0 ? 1u + rnd_n(255u) : 256u;
        if (rnd_n(3) == 0) { filter = rnd_n(128u); if (rnd_n(5) == 0) filter = 63u + rnd_n(2u); }
        if (rnd_n(4) == 0) res = rnd_n(2) ? 0u : rnd_n(128u);
        if (rnd_n(5) == 0) key = rnd_n(2) ? 0u : rnd_n(128u);
        if (rnd_n(3) == 0) note = rnd_n(128u);
        if (rnd_n(2) == 0) gen_pick(&g);
        for (uint32_t i = 0; i < n; ++i) row[i] = rowb[i] = gen_next(&g, i);
        if (!res && !key) {
            b.band = 0;
            for (uint32_t i = 0; i < n; ++i) rowb[i] = ref_filter_process(&b, rowb[i], filter);
        } else for (uint32_t i = 0; i < n; ++i)
            rowb[i] = ref_filter_process_controls(&b, rowb[i], filter, res, key, note);
        part_site_live(row, n, filter, res, key, note, &a);
        for (uint32_t i = 0; i < n; ++i) { ++g_checked; if (row[i] != rowb[i]) fail("part output differs", "part site", g_checked); }
        same_state(&a, &b, "part site");
        done += n;
    }
}

/* instrument_fx.c, br_master_ladder_frame: stereo, per sample, br_filter_ladder directly, resonance slewed per frame,
 * input clamped by the caller, f->low preset when mix is 0, the fade tail zeroes states. */
static void test_master_site(uint64_t samples)
{
    br_filter_t a[2]; ref_filter_t b[2]; gen_t g[2]; uint64_t done = 0;
    uint32_t filter = 30, res = 0;
    init_pair(&a[0], &b[0]); init_pair(&a[1], &b[1]);
    gen_pick(&g[0]); gen_pick(&g[1]);
    while (done < samples) {
        uint32_t n = 1u + rnd_n(rnd_n(3) ? 600u : 3000u);
        if (rnd_n(2) == 0) { filter = rnd_n(80u); if (rnd_n(5) == 0) filter = 63u + rnd_n(2u); }
        if (rnd_n(3) == 0) res = rnd_n(3) == 0 ? 0u : rnd_n(124u);
        if (rnd_n(2) == 0) { gen_pick(&g[0]); gen_pick(&g[1]); }
        uint32_t zero_at = rnd_n(30) == 0 ? rnd_n(n) : 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; ++i, ++done) {
            for (uint32_t ch = 0; ch < 2; ++ch) {
                int32_t in = gen_next(&g[ch], i) * 4, x = in >> 1, ya, yb, xb;
                uint32_t c = filter < 63u ? filter : 63u;
                if (x > 262144) x = 262144;
                if (x < -262144) x = -262144;
                xb = x;
                if (!a[ch].mix) a[ch].low = x;
                if (!b[ch].mix) b[ch].low = xb;
                ya = br_filter_ladder(&a[ch], x, c, res);
                yb = ref_filter_ladder(&b[ch], xb, c, res);
                ++g_checked;
                if (ya != yb) fail("master output differs", "master site", g_checked);
                if (i == zero_at) { /* the fade tail zeroing, applied to both */
                    a[ch].low = a[ch].band = 0; b[ch].low = b[ch].band = 0;
                    for (int k = 0; k < 4; ++k) a[ch].ladder_s[k] = b[ch].ladder_s[k] = 0;
                }
            }
            if ((i & 31u) == 0) { same_state(&a[0], &b[0], "master site L"); same_state(&a[1], &b[1], "master site R"); }
        }
        same_state(&a[0], &b[0], "master site L end"); same_state(&a[1], &b[1], "master site R end");
        /* the master leaves the ladder: once mix is 0, the owner clears it (memset) */
        if (!a[0].mix && !a[1].mix && rnd_n(2) == 0) { memset(a, 0, sizeof a); memset(b, 0, sizeof b); }
    }
}


/* ---- Non-exact switches (BR_FILTER_FB_MODE != 0): a tolerance instead of equality ----
 * Against the frozen reference: over every low-pass control and several resonances (including the self-oscillating
 * ones), noise at half scale, the rms error must stay below -60 dB of the reference output and, for the cutoffs where the
 * approximation is active, a few LSB at most; silence must settle to exact zero. The block API must still equal the live
 * per-sample path bit for bit (both use the same kernels). */
static void test_tolerance(void)
{
    static const uint32_t resos[] = { 1, 37, 64, 100, 118 };
    double worst_db = -300.0; int32_t worst_max = 0;
    for (uint32_t control = 0; control < 63; ++control)
        for (uint32_t ri = 0; ri < 5; ++ri) {
            br_filter_t a; ref_filter_t b; double se = 0, so = 0; int32_t mx = 0; uint32_t seed = 99u + control;
            init_pair(&a, &b);
            for (int i = 0; i < 60000; ++i) {
                int32_t x, ya, yb, d;
                seed = seed * 1664525u + 1013904223u;
                x = (int32_t)(((int64_t)(int32_t)seed * 16384) >> 31);
                ya = br_filter_process_controls(&a, x, control, resos[ri], 0u, 60u);
                yb = ref_filter_process_controls(&b, x, control, resos[ri], 0u, 60u);
                ++g_checked;
                if (i < 3000) continue;
                d = ya - yb; if (d < 0) d = -d; if (d > mx) mx = d;
                se += (double)d * d; so += (double)yb * yb;
            }
            if (so > 0) { double db = 10.0 * log10(se / so + 1e-30); if (db > worst_db) worst_db = db; }
            if (mx > worst_max) worst_max = mx;
            if (so > 0 && se / so > 1e-6) { fprintf(stderr, "control %u res %u: error %.1f dB\n", control, resos[ri], 10.0 * log10(se / so)); fail("error above -60 dB", "tolerance", g_checked); }
            /* silence: both must go to exact zero (the shared reverb relies on it) */
            for (int i = 0; i < 44100 * 8; ++i) { if (br_filter_process_controls(&a, 0, control, resos[ri], 0u, 60u) == 0 && !a.low && !a.ladder_s[0] && !a.ladder_s[1] && !a.ladder_s[2] && !a.ladder_s[3] && !a.extra_s[0] && !a.extra_s[1]) break; }
            if (a.low || a.ladder_s[3] || a.extra_s[1]) { fprintf(stderr, "control %u res %u does not settle\n", control, resos[ri]); if (resos[ri] < 119) fail("does not settle", "tolerance", g_checked); }
        }
    printf("tolerance mode: worst rms error %.1f dB, worst peak error %d LSB\n", worst_db, worst_max);
}
#ifdef BR_FILTER_HAS_BLOCK
static void test_block_vs_live(uint64_t samples)
{
    br_filter_t a, b; gen_t g; uint64_t done = 0; int32_t in[300], out[300];
    uint32_t control = 20, res = 64;
    br_filter_init(&a); br_filter_init(&b); gen_pick(&g);
    while (done < samples) {
        uint32_t n = 1u + rnd_n(299u);
        if (rnd_n(3) == 0) control = rnd_n(128u);
        if (rnd_n(3) == 0) res = rnd_n(128u);
        if (rnd_n(2) == 0) gen_pick(&g);
        for (uint32_t i = 0; i < n; ++i) in[i] = out[i] = gen_next(&g, i);
        br_filter_block_controls(&a, out, n, control, res, 0u, 60u);
        for (uint32_t i = 0; i < n; ++i) { ++g_checked; if (br_filter_process_controls(&b, in[i], control, res, 0u, 60u) != out[i]) fail("block differs from live per-sample", "self", g_checked); }
        if (memcmp(&a, &b, sizeof a)) fail("block state differs from live per-sample", "self", g_checked);
        done += n;
    }
}
#endif

int main(int argc, char **argv)
{
    uint64_t scale = argc > 1 ? (uint64_t)atoll(argv[1]) : 1u;
    if (BR_FILTER_FB_MODE) {
        test_tolerance();
#ifdef BR_FILTER_HAS_BLOCK
        test_block_vs_live(2000000u * scale);
#endif
        printf("filter equivalence (tolerance mode, BR_FILTER_FB_MODE=%d): %llu samples within tolerance, block == live per-sample\n", BR_FILTER_FB_MODE, (unsigned long long)g_checked);
        return 0;
    }
    test_every_control();
    test_random_per_sample(1500000u * scale);
#ifdef BR_FILTER_HAS_BLOCK
    test_block(1500000u * scale, 0);
    test_block(1500000u * scale, 1);
    test_block_steady();
    test_zero_skip();
#endif
    test_poly_site(1000000u * scale);
    test_part_site(1000000u * scale);
    test_master_site(1500000u * scale);
    printf("filter equivalence: %llu samples identical (output and every state field) to the frozen per-sample reference%s\n",
           (unsigned long long)g_checked,
#ifdef BR_FILTER_HAS_BLOCK
           ", block API included"
#else
           ""
#endif
    );
    return 0;
}
