/* SPDX-License-Identifier: GPL-3.0-only */
/* Frozen reference copy of the shared reverb (firmware/src/instrument_reverb2.h, _core.h and .c as at commit 56b61a4),
 * renamed rv2_* -> ref2_*, RV2_* -> REF2_*. tests/test_reverb2_equiv.c compares the live reverb with it sample for sample and
 * state for state. Do not edit. The options are the device build's: REF2_LINE_STRUCT=1. */
/* ref2: 8-line Hadamard FDN reverb for JamesOS (FM-1): the global reverb.
 * SPDX-License-Identifier: GPL-3.0-only
 * Firmware port of the host prototype (ref2.h, ref2_core.h,
 * ref2_ctl.c), kept textually identical except for the port hooks below (REF2_API,
 * REF2_TABLE, REF2_CORE_CALL) and these file names; tests/test_reverb2_golden.c
 * reproduces the prototype's golden hashes (out/golden.txt) bit for bit.
 * Written fresh. Structure (ideas only, no code borrowed): Jot/Stautner-Puckette
 * FDN with a Hadamard matrix as in ChoralRoot; per-line g^(L/Lref) gains (Jot);
 * input allpass diffusers as in the existing JamesOS reverb.
 *
 * Files
 *   instrument_reverb2.h       this header (prototype ref2.h)
 *   instrument_reverb2_core.h  (ref2_core.h) the per-sample loop: ONE static function, ref2_core_run(), no calls
 *               out (helpers are macros), meant for a RAM section (REF2_RAM)
 *   instrument_reverb2.c       (ref2_ctl.c) everything at block or control rate, can run from flash:
 *               ref2_design (control task, float + sqrtf only), ref2_apply (block
 *               start), ref2_process (driver: LFO/slew plan per 32 core samples,
 *               core call, end-of-tail bookkeeping), init/clear/idle.
 *
 * Rates: block API at 44.1 kHz (n even), core at 22.05 kHz.
 * Input: mono int32 send domain (clip +-250,000). Output: stereo int32 wet
 * return, clamped to +-65,535.
 *
 * MEMORY
 *   pool16  int16[REF2_POOL16_LEN = 12,328]  24,656 B  (budget 12,400 samples)
 *           8 lines (max+27 each) 9,638 | pre-delay 1,330 | swell APs 773+587
 *   pool32  int32[REF2_POOL32_LEN = 690]      2,760 B  (budget 1,340 words)
 *           input APs 113/83/283/211
 *   ref2_state_t (control, incl. applied coefficients)  REF2_STATE_BYTES
 *   ref2_coeffs_t (design output, control task)          REF2_COEFFS_BYTES
 */
#ifndef REF2_H
#define REF2_H
#include <stdint.h>

/* ---- compile-time CPU options ------------------------------------------- */
#ifndef REF2_UPSAMPLE_TAPS     /* 6: 11-tap half-band interpolator, 4: 7-tap */
#define REF2_UPSAMPLE_TAPS 6
#endif
#ifndef REF2_LINE_STRUCT       /* 1: per-line state as an array of structs */
#define REF2_LINE_STRUCT 0
#endif
#ifndef REF2_INT_READS         /* 1: lines 1,3,5,7 read at integer delay, unmodulated */
#define REF2_INT_READS 0
#endif
#ifndef REF2_RAM               /* firmware: __attribute__((section(".ram_audio"))) */
#define REF2_RAM
#endif
/* Port hooks (firmware: instrument_fx.c sets them before including instrument_reverb2.c).
 * REF2_API: linkage of the API functions (firmware: static, one translation unit).
 * REF2_TABLE(name): placement of the core's small constant tables (firmware: copied RAM
 * data, one section per table as -fdata-sections names a variable's).
 * REF2_CORE_CALL: how the block driver calls the core (firmware: an absolute indirect
 * call, because the driver runs from XIP flash and the core from RAM). */
#ifndef REF2_API
#define REF2_API
#endif
#ifndef REF2_TABLE
#define REF2_TABLE(name)
#endif
#ifndef REF2_CORE_CALL
#define REF2_CORE_CALL(fn, ...) fn(__VA_ARGS__)
#endif

/* ---- sizes -------------------------------------------------------------- */
#define REF2_LINES 8
#define REF2_L0 601
#define REF2_L1 743
#define REF2_L2 907
#define REF2_L3 1069
#define REF2_L4 1249
#define REF2_L5 1429
#define REF2_L6 1613
#define REF2_L7 1811
#define REF2_MOD_MAX 24
#define REF2_LINE_PAD (REF2_MOD_MAX + 3)
#define REF2_LINE_MEM (REF2_L0 + REF2_L1 + REF2_L2 + REF2_L3 + REF2_L4 + REF2_L5 + REF2_L6 + REF2_L7 + 8 * REF2_LINE_PAD)
#define REF2_PRE_LEN 1330
/* Additional full-rate line for synced short musical pre-delays, through 1/32 at 60 BPM. */
#define REF2_SYNC_PRE_LEN 2053
#define REF2_SYNC_PRE_COMP_LEN (REF2_SYNC_PRE_LEN * 2)
#define REF2_SW0_LEN 773
#define REF2_SW1_LEN 587
#define REF2_POOL16_LEN (REF2_LINE_MEM + REF2_PRE_LEN + REF2_SW0_LEN + REF2_SW1_LEN)
#define REF2_AP0_LEN 113
#define REF2_AP1_LEN 83
#define REF2_AP2_LEN 283
#define REF2_AP3_LEN 211
#define REF2_POOL32_LEN (REF2_AP0_LEN + REF2_AP1_LEN + REF2_AP2_LEN + REF2_AP3_LEN)
#define REF2_POOL16_BYTES (REF2_POOL16_LEN * 2)
#define REF2_POOL32_BYTES (REF2_POOL32_LEN * 4)
#define REF2_STATE_BYTES (sizeof(ref2_state_t))
#define REF2_COEFFS_BYTES (sizeof(ref2_coeffs_t))
#define REF2_CORE_FS 22050
#define REF2_SUB 32                     /* core samples per plan (64 frames) */

enum { REF2_ROOM = 0, REF2_HALL = 1, REF2_SPACE = 3 };

/* Optional user controls (save format extension). Each 0..127, or REF2_DERIVE
 * to derive from TYPE/SIZE/DECAY exactly as the mode tables do. */
#define REF2_DERIVE 255u
typedef struct {
    uint8_t pre;      /* pre-delay 0..60 ms (linear)                       */
    uint8_t tone;     /* RT(4k)/RT(1k) 0.25 (0) .. 0.95 (127)              */
    uint8_t mod;      /* modulation depth 0..20 core samples (linear)      */
    uint8_t lowcut;   /* in-loop low cut 10 Hz (0) .. 300 Hz (127), log    */
    uint8_t width;    /* 0 mono, 64 natural (uncorrelated), 127 extra wide */
} ref2_overrides_t;

/* Output of ref2_design, input of ref2_apply. */
typedef struct {
    int32_t target_q16[REF2_LINES];     /* read lengths, Q16 core samples */
    int32_t a[REF2_LINES], b[REF2_LINES], dd[REF2_LINES];   /* loop shelf, Q15 */
    int32_t hp_c;                      /* shared HP, Q15 */
    int32_t gin_q12, gout_q10, width_q15;
    int32_t ap_g[2], sw_g;             /* input diffusers (pairs), swell APs, Q15 */
    int32_t er_g, er_t[4];             /* early taps: Q12 gain, delays (core samples) */
    int32_t mod_depth_q8, pre_target;
    uint32_t lfo_inc[2], bloom, cap, rt_slow;
} ref2_coeffs_t;

#if REF2_LINE_STRUCT
typedef struct {
    int32_t d, inc, s, e, es, yap, a, b, dd;
    uint16_t wpos, off, blen, pad;
} ref2_line_t;
#define REF2_LN(st, i, f) ((st)->ln[i].f)
#else
#define REF2_LN(st, i, f) ((st)->f[i])
#endif

typedef struct {
    int16_t *p16;                      /* pools (placed by the caller) */
    int16_t *sync_pre;                  /* optional independent full-quality extension */
    uint16_t sync_pos, sync_d, sync_target;
    uint8_t sync_mode, sync_phase;
    uint16_t sync_pending;
    int16_t sync_hist[4];
    int32_t *p32;
#if REF2_LINE_STRUCT
    ref2_line_t ln[REF2_LINES];
#else
    int32_t d[REF2_LINES], inc[REF2_LINES];          /* read delay Q16 and per-sample ramp */
    int32_t s[REF2_LINES], e[REF2_LINES], es[REF2_LINES], yap[REF2_LINES];
    int32_t a[REF2_LINES], b[REF2_LINES], dd[REF2_LINES];
    uint16_t wpos[REF2_LINES];
#endif
    int32_t base[REF2_LINES], target[REF2_LINES];    /* slewed / target lengths Q16 */
    int32_t hp_lz, hp_e, hp_c;
    int32_t gin_q12, gout_q10, width_q15, ap_g[2], sw_g;
    int32_t er_g, er_d[4], er_t[4], er_l, er_r;
    uint32_t lfo_ph[2], lfo_inc[2];
    int32_t mod_depth_q8, pre_d, pre_target;
    uint16_t pre_pos, sw_pos[2], ap_pos[4];
    int32_t dec_h[11], up_l[6], up_r[6];           /* rate-conversion history */
    uint32_t bloom, end, loud, idle, configured;
    uint32_t quiet, since_input, cap, rt_slow, floor_blocks, blk_abs;
    int32_t blk_peak, peak_in;
    int32_t peak_line;                             /* host diagnostic only */
} ref2_state_t;

REF2_API void ref2_init(ref2_state_t *s, int16_t *pool16, int32_t *pool32);
REF2_API void ref2_design(unsigned type, unsigned size, unsigned decay, const ref2_overrides_t *ov, ref2_coeffs_t *c);
REF2_API void ref2_apply(ref2_state_t *s, const ref2_coeffs_t *c);
REF2_API void ref2_process(ref2_state_t *s, const int32_t *in, int32_t *out_l, int32_t *out_r, unsigned n);
REF2_API int ref2_idle(const ref2_state_t *s);
/* host helper: mid-band RT60 the tables ask for */
REF2_API float ref2_target_rt(unsigned type, unsigned decay);

#endif

/* ref2 per-sample loop. SPDX-License-Identifier: GPL-3.0-only
 * Include once, from the file that drives it (ref2_ctl.c). ref2_core_run() is a
 * single static function with no calls out; every helper below is a macro:
 *   REF2_TZ15      Q15 product rounded toward zero (allpasses, end mode)
 *   REF2_SAT16     clamp to +-32767
 *   REF2_LN        per-line field access (SoA or struct-of-lines, ref2.h)
 * Tables it reads: ref2_off[], ref2_blen[] (SoA layout only), ref2_ap_off/len[].
 * Integer only. Products are 32x32->32 and bounded as commented. */
#ifndef REF2_CORE_H
#define REF2_CORE_H

#define REF2_TZ15(p) (((p) + (((p) >> 31) & 32767)) >> 15)
#define REF2_SAT16(x) ((x) > 32767 ? 32767 : (x) < -32767 ? -32767 : (x))
#define REF2_KNEE 24575
#define REF2_OUT_MAX 65535

/* 8-bit logarithmic storage for the long pre-delay. Compact input keeps the
 * whole 1/8 note at 60 BPM in RAM; the short path remains 16-bit full-rate. */
static inline __attribute__((always_inline)) uint8_t ref2_sync_encode(int32_t x)
{
    uint32_t neg = x < 0, mag = (uint32_t)(x < 0 ? -x : x);
    uint32_t exponent, mantissa;
    mag += 132u; if (mag > 32767u) mag = 32767u;
    exponent = 31u - (uint32_t)__builtin_clz(mag | 255u) - 7u;
    mantissa = (mag >> (exponent + 3u)) & 15u;
    return (uint8_t)((neg << 7) | (exponent << 4) | mantissa);
}
static inline __attribute__((always_inline)) int32_t ref2_sync_decode(uint8_t v)
{
    int32_t mag = (int32_t)((((uint32_t)(v & 15u) << 3) + 132u) << ((v >> 4) & 7u)) - 132;
    return (v & 128u) ? -mag : mag;
}

#define REF2_O0 0
#define REF2_O1 (REF2_O0 + REF2_L0 + REF2_LINE_PAD)
#define REF2_O2 (REF2_O1 + REF2_L1 + REF2_LINE_PAD)
#define REF2_O3 (REF2_O2 + REF2_L2 + REF2_LINE_PAD)
#define REF2_O4 (REF2_O3 + REF2_L3 + REF2_LINE_PAD)
#define REF2_O5 (REF2_O4 + REF2_L4 + REF2_LINE_PAD)
#define REF2_O6 (REF2_O5 + REF2_L5 + REF2_LINE_PAD)
#define REF2_O7 (REF2_O6 + REF2_L6 + REF2_LINE_PAD)
#define REF2_OPRE (REF2_O7 + REF2_L7 + REF2_LINE_PAD)
#define REF2_OSW0 (REF2_OPRE + REF2_PRE_LEN)
#define REF2_OSW1 (REF2_OSW0 + REF2_SW0_LEN)
static const uint16_t ref2_off[REF2_LINES] REF2_TABLE(ref2_off) = {REF2_O0, REF2_O1, REF2_O2, REF2_O3, REF2_O4, REF2_O5, REF2_O6, REF2_O7};
static const uint16_t ref2_blen[REF2_LINES] REF2_TABLE(ref2_blen) = {
    REF2_L0 + REF2_LINE_PAD, REF2_L1 + REF2_LINE_PAD, REF2_L2 + REF2_LINE_PAD, REF2_L3 + REF2_LINE_PAD,
    REF2_L4 + REF2_LINE_PAD, REF2_L5 + REF2_LINE_PAD, REF2_L6 + REF2_LINE_PAD, REF2_L7 + REF2_LINE_PAD};
static const uint16_t ref2_ap_len[4] REF2_TABLE(ref2_ap_len) = {REF2_AP0_LEN, REF2_AP1_LEN, REF2_AP2_LEN, REF2_AP3_LEN};
static const uint16_t ref2_ap_off[4] REF2_TABLE(ref2_ap_off) = {0, REF2_AP0_LEN, REF2_AP0_LEN + REF2_AP1_LEN, REF2_AP0_LEN + REF2_AP1_LEN + REF2_AP2_LEN};
#if REF2_LINE_STRUCT
#define REF2_OFF(st, i) ((st)->ln[i].off)
#define REF2_BLEN(st, i) ((st)->ln[i].blen)
#else
#define REF2_OFF(st, i) (ref2_off[i])
#define REF2_BLEN(st, i) (ref2_blen[i])
#endif

/* Run `frames` input frames (even, <= 2*REF2_SUB) at 44.1 kHz. Sets s->loud
 * bit 0 when any line write exceeds 1 LSB, bit 1 when the network had input. */
static REF2_RAM void ref2_core_run(ref2_state_t *s, const int32_t *in, int32_t *out_l,
                                 int32_t *out_r, unsigned frames)
{
    int16_t *const mem = s->p16;
    const uint32_t end = s->end;
    uint32_t loud = s->loud;
    unsigned f;
    for (f = 0; f + 1u < frames; f += 2u) {
        int32_t r[8], v[8], o[8], x, i, ol, orr;
        /* ---- 2:1 decimation, 11-tap half-band (3, 0, -25, 0, 150, 256, ...)/512 */
        {
            int32_t *h = s->dec_h, x0 = in[f], x1 = in[f + 1u];
            x0 = x0 > 250000 ? 250000 : x0 < -250000 ? -250000 : x0;
            x1 = x1 > 250000 ? 250000 : x1 < -250000 ? -250000 : x1;
            for (i = 10; i >= 2; --i) h[i] = h[i - 2];   /* firmware: mirrored ring */
            h[1] = x0; h[0] = x1;
            /* |h| <= 250,000: 256*h + 150*2h ... < 2^28 */
            x = (256 * h[5] + 150 * (h[4] + h[6]) - 25 * (h[2] + h[8]) + 3 * (h[0] + h[10]) + 256) >> 9;
        }
        /* ---- send -> line domain (|x| < 2^18, gin_q12 < 2^12) */
        x = (x * s->gin_q12 + 2048) >> 12;
        x = REF2_SAT16(x);
        /* ---- short input diffusers, int32 pool, products toward zero */
        for (i = 0; i < 4; ++i) {
            int32_t *ab = s->p32 + ref2_ap_off[i];
            int32_t p = s->ap_pos[i], d = ab[p], g = s->ap_g[i >> 1];
            int32_t vv = x - REF2_TZ15(d * g);
            ab[p] = vv;
            x = d + REF2_TZ15(vv * g);
            if (++p == ref2_ap_len[i]) p = 0;
            s->ap_pos[i] = (uint16_t)p;
        }
        x = REF2_SAT16(x);
        /* The extra delay precedes the normal pre-delay and early reflections.
         * A zero extension is transparent, but the ring stays current so enabling
         * sync does not replay stale audio. */
        if (s->sync_pre) {
            uint32_t w = s->sync_pos, d = s->sync_d;
            if (!s->sync_mode) {
                int32_t delayed = x;
                if (d) delayed = s->sync_pre[w >= d ? w - d : w + REF2_SYNC_PRE_LEN - d];
                s->sync_pre[w] = (int16_t)x;
                if (++w == REF2_SYNC_PRE_LEN) w = 0u;
                s->sync_pos = (uint16_t)w;
                x = delayed;
            } else {
                /* 3:1, triangular anti-alias filter and linear reconstruction.
                 * The five input taps sum to nine; all arithmetic is bounded. */
                uint8_t *buf = (uint8_t *)s->sync_pre;
                int32_t h0 = s->sync_hist[0], h1 = s->sync_hist[1];
                int32_t h2 = s->sync_hist[2], h3 = s->sync_hist[3];
                uint32_t phase = s->sync_phase, age, q, rem, a, b;
                /* Do not let the tail manager clear audio still in this long
                 * delay while the network is waiting for it to arrive. */
                if (x) s->sync_pending = (uint16_t)(d + 1u);
                else if (s->sync_pending) --s->sync_pending;
                if (s->sync_pending) loud |= 3u;
                if (!phase) {
                    int32_t filtered = (x + 2*h0 + 3*h1 + 2*h2 + h3) / 9;
                    buf[w] = ref2_sync_encode(filtered);
                    if (++w == REF2_SYNC_PRE_COMP_LEN) w = 0u;
                    s->sync_pos = (uint16_t)w;
                }
                s->sync_hist[3] = (int16_t)h2; s->sync_hist[2] = (int16_t)h1;
                s->sync_hist[1] = (int16_t)h0; s->sync_hist[0] = (int16_t)x;
                s->sync_phase = (uint8_t)(phase == 2u ? 0u : phase + 1u);
                /* The filter itself contributes two core samples of delay. */
                age = d > phase + 2u ? d - phase - 2u : 0u;
                q = age / 3u; rem = age - q * 3u;
                a = w > q ? w - 1u - q : w + REF2_SYNC_PRE_COMP_LEN - 1u - q;
                b = a ? a - 1u : REF2_SYNC_PRE_COMP_LEN - 1u;
                x = (ref2_sync_decode(buf[a]) * (int32_t)(3u - rem) + ref2_sync_decode(buf[b]) * (int32_t)rem) / 3;
            }
        }
        /* ---- pre-delay (holds the diffused input) + early-reflection taps */
        {
            const int16_t *pb = mem + REF2_OPRE;
            int32_t w = s->pre_pos, ri = w - s->pre_d;
            if (ri < 0) ri += REF2_PRE_LEN;
            mem[REF2_OPRE + w] = (int16_t)x;
            x = pb[ri];
            if (s->er_g) {
                int32_t t0, t1, t2, t3, j;
                j = w - s->er_d[0]; if (j < 0) j += REF2_PRE_LEN; t0 = pb[j];
                j = w - s->er_d[1]; if (j < 0) j += REF2_PRE_LEN; t1 = pb[j];
                j = w - s->er_d[2]; if (j < 0) j += REF2_PRE_LEN; t2 = pb[j];
                j = w - s->er_d[3]; if (j < 0) j += REF2_PRE_LEN; t3 = pb[j];
                s->er_l = ((t0 + t1 - t2 + t3) * s->er_g) >> 10;   /* Q12 gain -> Q2 */
                s->er_r = ((t0 - t1 + t2 + t3) * s->er_g) >> 10;
            }
            if (++w == REF2_PRE_LEN) w = 0;
            s->pre_pos = (uint16_t)w;
        }
        /* ---- long swell allpasses, int16 pool (HALL/SPACE) */
        if (s->sw_g) {
            int32_t k;
            for (k = 0; k < 2; ++k) {
                int16_t *ab = mem + (k ? REF2_OSW1 : REF2_OSW0);
                int32_t p = s->sw_pos[k], d = ab[p], vv;
                vv = x - REF2_TZ15(d * s->sw_g);
                vv = REF2_SAT16(vv);
                ab[p] = (int16_t)vv;
                x = d + REF2_TZ15(vv * s->sw_g);
                if (++p == (k ? REF2_SW1_LEN : REF2_SW0_LEN)) p = 0;
                s->sw_pos[k] = (uint16_t)p;
            }
        }
        if (x) loud |= 3u;
        /* ---- line reads, Q2. First-order allpass fractional delay:
         * D = N + 0.5 + frac, eta = (0.5 - frac)/2 (Q15, |eta| <= 1/4),
         * y = eta (x[n-N] - y[-1]) + x[n-N-1]; unit magnitude at all eta. */
        for (i = 0; i < 8; ++i) {
            const int16_t *buf = mem + REF2_OFF(s, i);
            int32_t blen = REF2_BLEN(s, i), w = REF2_LN(s, i, wpos), dq = REF2_LN(s, i, d);
#if REF2_INT_READS
            if (i & 1) {
                int32_t i0 = w - ((dq + 32768) >> 16);
                if (i0 < 0) i0 += blen;
                r[i] = 4 * buf[i0];
            } else
#endif
            {
                int32_t dd = dq - 32768, N = dd >> 16, eta = (32768 - (dd & 0xFFFF)) >> 2;
                int32_t i0 = w - N, i1, y;
                if (i0 < 0) i0 += blen;
                i1 = i0 - 1; if (i1 < 0) i1 += blen;
                /* eta < 2^13, (4 buf - y) < 2^18 */
                y = ((eta * (4 * buf[i0] - REF2_LN(s, i, yap)) + 16384) >> 15) + 4 * buf[i1];
                REF2_LN(s, i, yap) = y;
                r[i] = y;
            }
            REF2_LN(s, i, d) = dq + REF2_LN(s, i, inc);
        }
        /* ---- outputs: orthogonal sign sets over all 8 lines; bloom: lines 4..7 */
        if (!s->bloom) {
            ol  = s->er_l + r[0] + r[1] - r[2] + r[3] - r[4] + r[5] + r[6] - r[7];
            orr = s->er_r + r[0] - r[1] + r[2] + r[3] + r[4] + r[5] - r[6] - r[7];
        } else {
            ol  = r[4] + r[5] - r[6] + r[7];
            orr = r[4] - r[5] + r[6] + r[7];
        }

        /* ---- loop shelf H = Gh + (Gl-Gh)(1-p)/(1-p z^-1), 1/sqrt8 folded in.
         * State s Q2. |b r|,|a s|,|dd r| < 2^30.5; halved before subtracting.
         * Normal: first-order error feedback. End: truncate new state to zero. */
        if (!end) {
            for (i = 0; i < 8; ++i) {
                int32_t acc = ((REF2_LN(s, i, b) * r[i]) >> 1) - ((REF2_LN(s, i, a) * REF2_LN(s, i, s)) >> 1) + REF2_LN(s, i, e);
                int32_t q = (acc + 8192) >> 14;
                REF2_LN(s, i, e) = acc - (q << 14);
                REF2_LN(s, i, s) += q;
                o[i] = REF2_LN(s, i, s) + ((REF2_LN(s, i, dd) * r[i] + 16384) >> 15);
            }
        } else {
            for (i = 0; i < 8; ++i) {
                int32_t t = (REF2_LN(s, i, s) << 14) + ((REF2_LN(s, i, b) * r[i]) >> 1) - ((REF2_LN(s, i, a) * REF2_LN(s, i, s)) >> 1);
                REF2_LN(s, i, s) = (t + ((t >> 31) & 16383)) >> 14;
                REF2_LN(s, i, e) = 0;
                o[i] = REF2_LN(s, i, s) + REF2_TZ15(REF2_LN(s, i, dd) * r[i]);
            }
        }
        /* ---- 8-point fast Walsh-Hadamard transform (unnormalised) */
        {
            int32_t a0 = o[0] + o[1], a1 = o[0] - o[1], a2 = o[2] + o[3], a3 = o[2] - o[3];
            int32_t a4 = o[4] + o[5], a5 = o[4] - o[5], a6 = o[6] + o[7], a7 = o[6] - o[7];
            int32_t b0 = a0 + a2, b2 = a0 - a2, b1 = a1 + a3, b3 = a1 - a3;
            int32_t b4 = a4 + a6, b6 = a4 - a6, b5 = a5 + a7, b7 = a5 - a7;
            v[0] = b0 + b4; v[4] = b0 - b4; v[1] = b1 + b5; v[5] = b1 - b5;
            v[2] = b2 + b6; v[6] = b2 - b6; v[3] = b3 + b7; v[7] = b3 - b7;
        }
        /* ---- shared high-pass on line 0: y = (1-c/2)(x-lz), lz += c(x-lz).
         * |v0| < 2^19, hp_c < 2^13. Bypassed and cleared in end mode. */
        if (!end) {
            int32_t dlt = v[0] - s->hp_lz, acc = s->hp_c * dlt + s->hp_e, q = (acc + 16384) >> 15;
            s->hp_e = acc - (q << 15);
            s->hp_lz += q;
            v[0] = dlt - (q >> 1);
        } else s->hp_lz = s->hp_e = 0;
        /* ---- inject input (sign pattern is not a Hadamard row) */
        x *= 4;
        v[0] += x; v[1] -= x; v[2] += x; v[3] += x;
        if (!s->bloom) { v[4] -= x; v[5] += x; v[6] -= x; v[7] -= x; }
        /* ---- Q2 -> int16 with error feedback (end: toward zero), soft knee, store */
        for (i = 0; i < 8; ++i) {
            int32_t w, p;
            if (!end) { int32_t t = v[i] + REF2_LN(s, i, es); w = (t + 2) >> 2; REF2_LN(s, i, es) = t - 4 * w; }
            else w = (v[i] + ((v[i] >> 31) & 3)) >> 2;
            if (w > REF2_KNEE) { int32_t ex = w - REF2_KNEE; w = ex >= 16384 ? 32767 : REF2_KNEE + ex - ((ex * ex) >> 15); }
            else if (w < -REF2_KNEE) { int32_t ex = -REF2_KNEE - w; w = ex >= 16384 ? -32767 : -REF2_KNEE - ex + ((ex * ex) >> 15); }
            loud |= (uint32_t)(w + 1) > 2u;
#ifdef REF2_HOST_DIAG
            { int32_t aw = (v[i] < 0 ? -v[i] : v[i]) >> 2; if (aw > s->peak_line) s->peak_line = aw; }
#endif
            p = REF2_LN(s, i, wpos);
            mem[REF2_OFF(s, i) + p] = (int16_t)w;
            if (++p == REF2_BLEN(s, i)) p = 0;
            REF2_LN(s, i, wpos) = (uint16_t)p;
        }
        { int32_t a0 = v[0] < 0 ? -v[0] : v[0]; if (a0 > s->blk_peak) s->blk_peak = a0; s->blk_abs += (uint32_t)a0; }
        /* ---- output gain, then width (|l| < 2^18, gout_q10 < 2^12, |width| <= 2^15) */
        {
            int32_t l = (ol + 2) >> 2, rr = (orr + 2) >> 2, lw, rw, m0, m1;
            l = (l * s->gout_q10 + 512) >> 10;
            rr = (rr * s->gout_q10 + 512) >> 10;
            lw = l + ((s->width_q15 * (rr >> 5)) >> 10);
            rw = rr + ((s->width_q15 * (l >> 5)) >> 10);
            /* ---- 1:2 interpolation: frame 0 = half-sample point, frame 1 = centre */
            {
                int32_t *a = s->up_l, *b = s->up_r;
                for (i = 5; i >= 1; --i) { a[i] = a[i - 1]; b[i] = b[i - 1]; }
                a[0] = lw; b[0] = rw;
#if REF2_UPSAMPLE_TAPS == 4
                m0 = (9 * (a[2] + a[3]) - (a[1] + a[4]) + 8) >> 4;
                m1 = (9 * (b[2] + b[3]) - (b[1] + b[4]) + 8) >> 4;
#else
                m0 = (150 * (a[2] + a[3]) - 25 * (a[1] + a[4]) + 3 * (a[0] + a[5]) + 128) >> 8;
                m1 = (150 * (b[2] + b[3]) - 25 * (b[1] + b[4]) + 3 * (b[0] + b[5]) + 128) >> 8;
#endif
                m0 = m0 > REF2_OUT_MAX ? REF2_OUT_MAX : m0 < -REF2_OUT_MAX ? -REF2_OUT_MAX : m0;
                m1 = m1 > REF2_OUT_MAX ? REF2_OUT_MAX : m1 < -REF2_OUT_MAX ? -REF2_OUT_MAX : m1;
                out_l[f] = m0; out_r[f] = m1;
                m0 = a[2] > REF2_OUT_MAX ? REF2_OUT_MAX : a[2] < -REF2_OUT_MAX ? -REF2_OUT_MAX : a[2];
                m1 = b[2] > REF2_OUT_MAX ? REF2_OUT_MAX : b[2] < -REF2_OUT_MAX ? -REF2_OUT_MAX : b[2];
                out_l[f + 1u] = m0; out_r[f + 1u] = m1;
            }
        }
    }
    s->loud = loud;
}
#endif

/* ref2 control/block-rate side. SPDX-License-Identifier: GPL-3.0-only
 * ref2_design: control task. Float arithmetic, the only library call is sqrtf
 *   (exp2/log2 are local polynomials). About 4k flops per call (see report).
 * ref2_apply / ref2_process / end-of-tail bookkeeping: block rate, flash. */
#include <math.h>     /* sqrtf only */
#include <string.h>

/* ---- per-mode tables ---------------------------------------------------- */
typedef struct {
    float rt0, rt64, rt127;     /* mid-band (1 kHz) RT60 at DECAY 0/64/127, s */
    float sc0, sc127;           /* line length scale over SIZE */
    float pre0, pre127;         /* pre-delay ms over SIZE */
    float hf0, hf127;           /* RT(4k)/RT(1k) over SIZE */
    float lf;                   /* RT(DC)/RT(1k) before the shared high-pass */
    float lowcut;               /* shared in-loop HP corner, Hz */
    float mod_lo, mod_hi;       /* LFO depth (core samples) at DECAY <= 64 / >= 96 */
    float mod_rate;             /* Hz (second LFO at 1.37x) */
    float apg_a, apg_b;         /* short input diffusers, pairs 0/1 and 2/3 */
    float swell;                /* long input allpasses (0 = bypass) */
    int bloom;                  /* 1: input to lines 0-3, output from 4-7 */
    float width_k;              /* L/R cross-feed (+ narrows, - widens) */
    float er_gain, er_ms;       /* early taps: gain, span at line scale 1 */
    float gin, gout;            /* send -> line, line sum -> wet return */
    float lvl0, lvl64, lvl127;  /* input trim (dB) over SIZE */
    float comp;                 /* level compensation exponent vs DECAY */
} ref2_mode_t;

static const ref2_mode_t ref2_modes[4] = {
    [0] = { .rt0 = 0.25f, .rt64 = 0.9f, .rt127 = 6.0f, .sc0 = 0.38f, .sc127 = 0.68f,
            .pre0 = 0.0f, .pre127 = 12.0f, .hf0 = 0.75f, .hf127 = 0.65f, .lf = 1.10f,
            .lowcut = 30.0f, .mod_lo = 8.0f, .mod_hi = 12.0f, .mod_rate = 0.35f,
            .apg_a = 0.60f, .apg_b = 0.50f, .swell = 0.0f, .bloom = 0, .width_k = 0.12f,
            .er_gain = 1.5f, .er_ms = 30.0f, .gin = 0.0355f, .gout = 2.40f,
            .lvl0 = -1.9f, .lvl64 = 0.4f, .lvl127 = 0.7f, .comp = 0.30f },
    [1] = { .rt0 = 0.8f, .rt64 = 2.8f, .rt127 = 20.0f, .sc0 = 0.55f, .sc127 = 1.00f,
            .pre0 = 10.0f, .pre127 = 45.0f, .hf0 = 0.50f, .hf127 = 0.42f, .lf = 1.20f,
            .lowcut = 40.0f, .mod_lo = 8.0f, .mod_hi = 11.0f, .mod_rate = 0.40f,
            .apg_a = 0.70f, .apg_b = 0.60f, .swell = 0.50f, .bloom = 0, .width_k = 0.05f,
            .er_gain = 0.5f, .er_ms = 25.0f, .gin = 0.0265f, .gout = 2.40f,
            .lvl0 = 0.3f, .lvl64 = 0.1f, .lvl127 = 1.0f, .comp = 0.30f },
    [3] = { .rt0 = 2.5f, .rt64 = 8.0f, .rt127 = 60.0f, .sc0 = 0.75f, .sc127 = 1.00f,
            .pre0 = 20.0f, .pre127 = 60.0f, .hf0 = 0.38f, .hf127 = 0.32f, .lf = 1.00f,
            .lowcut = 100.0f, .mod_lo = 16.0f, .mod_hi = 16.0f, .mod_rate = 0.35f,
            .apg_a = 0.70f, .apg_b = 0.60f, .swell = 0.70f, .bloom = 1, .width_k = -0.08f,
            .er_gain = 0.0f, .er_ms = 0.0f, .gin = 0.0355f, .gout = 3.39f,
            .lvl0 = -0.4f, .lvl64 = -0.9f, .lvl127 = 0.5f, .comp = 0.30f },
};
static const uint16_t ref2_len[REF2_LINES] = {REF2_L0, REF2_L1, REF2_L2, REF2_L3, REF2_L4, REF2_L5, REF2_L6, REF2_L7};

static const ref2_mode_t *refx_mode_of(unsigned t) { return &ref2_modes[(t == 1u || t == 3u) ? t : 0u]; }

/* Helpers called from many places in ref2_design (control rate, flash). Out of line
 * they exist once; the vendor compiler otherwise inlines every call even under -Oz.
 * Strict floating point (no contraction, no fast math), so results are unchanged. */
#ifndef REF2_OUTLINE
#define REF2_OUTLINE __attribute__((noinline, minsize))
#endif

/* ---- local exp2/log2 (no libm beyond sqrtf); rel. error < 2e-7 ---------- */
typedef union { float f; uint32_t u; } ref2_fu;
static REF2_OUTLINE float ref2_log2(float x)
{
    ref2_fu v; float m, t, t2;
    int e;
    if (!(x > 0.0f)) return -150.0f;
    v.f = x;
    e = (int)((v.u >> 23) & 255u) - 127;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
    m = v.f;
    if (m > 1.41421356f) { m *= 0.5f; ++e; }
    t = (m - 1.0f) / (m + 1.0f); t2 = t * t;
    return (float)e + 2.88539008f * t * (1.0f + t2 * (0.33333333f + t2 * (0.2f + t2 * (0.14285714f + t2 * 0.11111111f))));
}
static REF2_OUTLINE float ref2_exp2(float x)
{
    ref2_fu v; int i; float y;
    if (x < -126.0f) return 0.0f;
    if (x > 126.0f) x = 126.0f;
    i = (int)x; if ((float)i > x) --i;
    y = (x - (float)i) * 0.69314718f;
    v.u = (uint32_t)(i + 127) << 23;
    return v.f * (1.0f + y * (1.0f + y * (0.5f + y * (0.16666667f + y * (0.041666667f + y * (0.0083333333f
                 + y * (0.0013888889f + y * (0.00019841270f + y * 0.000024801587f))))))));
}
#define LOG2_10 3.32192809f
#define LOG2_E 1.44269504f
/* trig of the design frequencies at 22,050 Hz (constants, no sin/cos at run time) */
#define C1K 0.95967523f
#define S1K 0.28111111f
#define SH1K 0.14199432f   /* sin(w/2) at 1 kHz */
#define C3K 0.65632781f
#define S3K 0.75447585f

static float refx_rt_of(const ref2_mode_t *m, unsigned decay)
{
    /* quadratic in log2(RT) through (0,rt0), (64,rt64), (127,rt127) */
    float y0 = ref2_log2(m->rt0), y1 = ref2_log2(m->rt64), y2 = ref2_log2(m->rt127);
    float s1 = (y1 - y0) / 64.0f, s2 = (y2 - y1) / 63.0f;
    float c = (s2 - s1) / 127.0f, b = s1 - c * 64.0f, d = (float)decay;
    return ref2_exp2(y0 + b * d + c * d * d);
}
REF2_API float ref2_target_rt(unsigned type, unsigned decay) { return refx_rt_of(refx_mode_of(type), decay); }

static unsigned refx_gcd_u(unsigned a, unsigned b) { while (b) { unsigned t = a % b; a = b; b = t; } return a; }

static float refx_held_energy(float rt)   /* energy of a 2 s held note, for level compensation */
{
    float tau = rt / (6.0f * 2.302585f);
    return tau * (1.0f - ref2_exp2(-2.0f / tau * LOG2_E));
}

/* P(w) = (1-p)/(1 - p e^-jw) for the two design frequencies */
static void refx_pw(float p, float c, float sn, float *pr, float *pi)
{
    float dr = 1.0f - p * c, di = p * sn, den = dr * dr + di * di;
    *pr = (1.0f - p) * dr / den; *pi = -(1.0f - p) * di / den;
}
/* Gh such that |Gl P1 + Gh (1-P1)| = g at 1 kHz; returns -1 if none in [0, Gl] */
static REF2_OUTLINE float refx_solve_gh(float p, float Gl, float g)
{
    float pr, pi, Ar, Ai, Br, Bi, qa, qb, qc, disc, gh;
    refx_pw(p, C1K, S1K, &pr, &pi);
    Ar = Gl * pr; Ai = Gl * pi; Br = 1.0f - pr; Bi = -pi;
    qa = Br * Br + Bi * Bi; qb = 2.0f * (Ar * Br + Ai * Bi); qc = Ar * Ar + Ai * Ai - g * g;
    disc = qb * qb - 4.0f * qa * qc;
    if (qa < 1e-12f || disc < 0.0f) return -1.0f;
    gh = (-qb + sqrtf(disc)) / (2.0f * qa);
    if (gh > Gl && gh < Gl * 1.002f) gh = Gl;          /* flat shelf: rounding */
    return (gh < 0.0f || gh > Gl) ? -1.0f : gh;
}
/* cost of pole p: |hf_got - hf|/hf, for mid gain g (log2 lg) and DC gain Gl.
 * About 65 flops, 2 divisions, 2 sqrtf. */
static REF2_OUTLINE float refx_shelf_cost(float p, float lg, float g, float Gl, float hf)
{
    float gh = refx_solve_gh(p, Gl, g), pr, pi, hr, hi, h3, l3, d;
    if (gh < 0.0f) return 1e9f;
    refx_pw(p, C3K, S3K, &pr, &pi);
    hr = Gl * pr + gh * (1.0f - pr); hi = (Gl - gh) * pi;
    h3 = sqrtf(hr * hr + hi * hi);
    l3 = ref2_log2(h3 < 0.99999f ? h3 : 0.99999f);
    d = lg / l3 - hf;
    return (d < 0.0f ? -d : d) / hf;
}

REF2_API void ref2_design(unsigned type, unsigned size, unsigned decay, const ref2_overrides_t *ov, ref2_coeffs_t *c)
{
    const ref2_mode_t *m = refx_mode_of(type);
    static const ref2_overrides_t none = {REF2_DERIVE, REF2_DERIVE, REF2_DERIVE, REF2_DERIVE, REF2_DERIVE};
    unsigned i, j, len[REF2_LINES];
    float fs = (float)REF2_CORE_FS, u, rt, sc, hf, lowcut, depth, pre_ms, wk, Lref = 0.0f;
    if (!ov) ov = &none;
    if (size > 127u) size = 127u;
    if (decay > 127u) decay = 127u;
    u = (float)size / 127.0f;
    rt = refx_rt_of(m, decay);
    sc = m->sc0 + (m->sc127 - m->sc0) * u;
    hf = ov->tone <= 127u ? 0.25f + 0.70f * (float)ov->tone / 127.0f : m->hf0 + (m->hf127 - m->hf0) * u;
    lowcut = ov->lowcut <= 127u ? 10.0f * ref2_exp2((float)ov->lowcut / 127.0f * 4.9068906f) : m->lowcut;
    if (ov->mod <= 127u) depth = 20.0f * (float)ov->mod / 127.0f;
    else {
        float t = decay <= 64u ? 0.0f : decay >= 96u ? 1.0f : (float)(decay - 64u) / 32.0f;
        depth = m->mod_lo + (m->mod_hi - m->mod_lo) * t;
    }
    pre_ms = ov->pre <= 127u ? 60.0f * (float)ov->pre / 127.0f : m->pre0 + (m->pre127 - m->pre0) * u;
    if (ov->width <= 127u)
        wk = ov->width <= 64u ? (float)(64u - ov->width) / 64.0f : -0.25f * (float)(ov->width - 64u) / 63.0f;
    else wk = m->width_k;

    /* read lengths, mutually prime */
    for (i = 0; i < REF2_LINES; ++i) {
        unsigned l = (unsigned)((float)ref2_len[i] * sc + 0.5f);
        if (l < 40u) l = 40u;
        for (;;) {
            for (j = 0; j < i; ++j) if (refx_gcd_u(l, len[j]) != 1u) break;
            if (j == i) break;
            ++l;
        }
        if (l > ref2_len[i]) l = ref2_len[i];
        len[i] = l; Lref += (float)l;
        c->target_q16[i] = (int32_t)(l << 16);
    }
    Lref *= 1.0f / REF2_LINES;

    /* shared HP on line 0: y = (1-c/2)(1-z^-1)/(1-(1-c)z^-1); its 1 kHz loss is
     * spread over the eight line gains (energy-average equivalent) */
    float hc = 1.0f - ref2_exp2(-6.2831853f * lowcut / fs * LOG2_E);
    c->hp_c = (int32_t)(hc * 32768.0f + 0.5f);
    float hp1k, hp_lcomp;
    {
        float num = 2.0f * SH1K * (1.0f - 0.5f * hc), re = 1.0f - (1.0f - hc) * C1K, im = (1.0f - hc) * S1K;
        hp1k = num / sqrtf(re * re + im * im);
        hp_lcomp = -ref2_log2(hp1k) / 8.0f;                 /* log2 of the per-line make-up */
    }

    /* Loop shelf H = Gh + (Gl-Gh)(1-p)/(1-p z^-1), designed once on the mean
     * line length: |H(1k)| = g (mid RT), |H(3k)| = g^(1/hf), H(0) = Gl = g^(1/(lf k)).
     * k in {1, 1.25, 1.6}: a higher LF RT is accepted when it buys the HF
     * target (cost = HF miss + 0.5 (k-1)). p: 8-point grid + 10 golden steps.
     * Per line: same p and lf k, gains scaled to that line's length, Gh
     * re-solved so each line's mid gain is exact. */
    float lg_ref = -3.0f * Lref * LOG2_10 / (rt * fs);
    float best_p = 0.5f, best_k = 1.0f, best_cost = 1e9f;
    {
        static const float ks[3] = {1.0f, 1.25f, 1.6f};
        unsigned kk;
        for (kk = 0; kk < 3u; ++kk) {
            float lfk = m->lf * ks[kk], extra = 0.5f * (ks[kk] - 1.0f), lo, hi, bp = 0.5f, bc = 1e9f;
            float gr = ref2_exp2(lg_ref), Glr = ref2_exp2(lg_ref / lfk);
            for (j = 0; j < 8u; ++j) {
                float p = 0.06f + 0.12f * (float)j, cst = refx_shelf_cost(p, lg_ref, gr, Glr, hf);
                if (cst < bc) { bc = cst; bp = p; }
            }
            lo = bp - 0.12f; hi = bp + 0.12f;
            if (lo < 0.0f) lo = 0.0f;
            if (hi > 0.975f) hi = 0.975f;
            {
                float x1 = hi - 0.618034f * (hi - lo), x2 = lo + 0.618034f * (hi - lo);
                float f1 = refx_shelf_cost(x1, lg_ref, gr, Glr, hf), f2 = refx_shelf_cost(x2, lg_ref, gr, Glr, hf);
                for (j = 0; j < 10u; ++j) {
                    if (f1 < f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - 0.618034f * (hi - lo); f1 = refx_shelf_cost(x1, lg_ref, gr, Glr, hf); }
                    else { lo = x1; x1 = x2; f1 = f2; x2 = lo + 0.618034f * (hi - lo); f2 = refx_shelf_cost(x2, lg_ref, gr, Glr, hf); }
                }
                if (f1 < bc) { bc = f1; bp = x1; }
                if (f2 < bc) { bc = f2; bp = x2; }
            }
            if (bc + extra < best_cost) { best_cost = bc + extra; best_p = bp; best_k = lfk; }
        }
    }
    if (best_cost >= 1e8f) { best_p = 0.0f; best_k = m->lf; }   /* no shelf fits: flat loop filter */
    float rt_dc_max = rt, mk = ref2_exp2(hp_lcomp);
    for (i = 0; i < REF2_LINES; ++i) {
        float L = (float)len[i], lg = lg_ref * L / Lref, g = ref2_exp2(lg);
        float Gl = ref2_exp2(lg / best_k), p = best_p, gh;
        if (Gl > 0.9990f) Gl = 0.9990f;
        gh = refx_solve_gh(p, Gl, g);
        if (gh < 0.0f) {   /* fall back to a plain low-pass with exact mid gain */
            float pr, pi; refx_pw(p, C1K, S1K, &pr, &pi);
            gh = 0.0f; Gl = g / sqrtf(pr * pr + pi * pi);
            if (Gl > 0.9990f) Gl = 0.9990f;
        }
        c->a[i] = (int32_t)((1.0f - p) * 32768.0f + 0.5f);
        c->b[i] = (int32_t)((Gl - gh) * mk * (1.0f - p) * 32768.0f * 0.35355339f + 0.5f);
        c->dd[i] = (int32_t)(gh * mk * 32768.0f * 0.35355339f + 0.5f);
        {
            float glc = Gl * mk; if (glc > 0.9995f) glc = 0.9995f;
            float rt_dc = -3.0f * L * LOG2_10 / (fs * ref2_log2(glc));
            if (rt_dc > rt_dc_max) rt_dc_max = rt_dc;
        }
    }

    /* level: input gain with SIZE trim and DECAY compensation (held 2 s note) */
    {
        float lvl = size <= 64u ? m->lvl0 + (m->lvl64 - m->lvl0) * (float)size / 64.0f
                                : m->lvl64 + (m->lvl127 - m->lvl64) * (float)(size - 64u) / 63.0f;
        float comp = ref2_exp2(m->comp * ref2_log2(refx_held_energy(refx_rt_of(m, 64u)) / refx_held_energy(rt)));
        float gin = m->gin * ref2_exp2(lvl / 20.0f * LOG2_10) * comp;
        c->gin_q12 = (int32_t)(gin * 4096.0f + 0.5f);
    }
    c->gout_q10 = (int32_t)(m->gout * 1024.0f + 0.5f);
    c->width_q15 = (int32_t)(wk * 32768.0f + (wk < 0.0f ? -0.5f : 0.5f));
    if (c->width_q15 > 32768) c->width_q15 = 32768;
    c->ap_g[0] = (int32_t)(m->apg_a * 32768.0f + 0.5f);
    c->ap_g[1] = (int32_t)(m->apg_b * 32768.0f + 0.5f);
    c->sw_g = (int32_t)(m->swell * 32768.0f + 0.5f);
    c->bloom = (uint32_t)m->bloom;
    if (depth > (float)REF2_MOD_MAX - 1.0f) depth = (float)REF2_MOD_MAX - 1.0f;
    c->mod_depth_q8 = (int32_t)(depth * 256.0f + 0.5f);
    c->lfo_inc[0] = (uint32_t)(m->mod_rate / fs * 4294967296.0f);
    c->lfo_inc[1] = (uint32_t)(m->mod_rate * 1.37f / fs * 4294967296.0f);
    c->pre_target = (int32_t)(pre_ms * 0.001f * fs + 0.5f);
    if (c->pre_target > REF2_PRE_LEN - 2) c->pre_target = REF2_PRE_LEN - 2;
    c->er_g = (int32_t)(m->er_gain * 4096.0f + 0.5f);
    {
        static const float frac[4] = {0.17f, 0.39f, 0.64f, 1.0f};
        for (i = 0; i < 4u; ++i) {
            int32_t t = c->pre_target + (int32_t)(m->er_ms * 0.001f * fs * sc * frac[i] + 0.5f);
            c->er_t[i] = t > REF2_PRE_LEN - 1 ? REF2_PRE_LEN - 1 : t;
        }
    }
    /* hard cap 1.6 RT (-96 dB) of the slowest band + 0.3 s; slowest RT in samples */
    c->cap = (uint32_t)((1.6f * rt_dc_max + 0.3f) * fs);
    c->rt_slow = (uint32_t)(rt_dc_max * fs);
}

/* ---- state -------------------------------------------------------------- */
static void ref2_clear(ref2_state_t *s)
{
    unsigned i;
    memset(s->p16, 0, REF2_POOL16_BYTES);
    memset(s->p32, 0, REF2_POOL32_BYTES);
    if (s->sync_pre) memset(s->sync_pre, 0, REF2_SYNC_PRE_LEN * sizeof *s->sync_pre);
    memset(s->sync_hist, 0, sizeof s->sync_hist);
    s->sync_phase = 0u; s->sync_pos = 0u; s->sync_pending = 0u;
    for (i = 0; i < REF2_LINES; ++i) {
        REF2_LN(s, i, s) = REF2_LN(s, i, e) = REF2_LN(s, i, es) = REF2_LN(s, i, yap) = 0;
    }
    s->hp_lz = s->hp_e = 0;
    memset(s->dec_h, 0, sizeof s->dec_h);
    memset(s->up_l, 0, sizeof s->up_l);
    memset(s->up_r, 0, sizeof s->up_r);
    s->er_l = s->er_r = 0;
    s->quiet = s->since_input = s->floor_blocks = s->blk_abs = 0u;
    s->blk_peak = s->peak_in = 0;
    s->end = 0u;
    s->idle = 1u;
}

REF2_API void ref2_init(ref2_state_t *s, int16_t *pool16, int32_t *pool32)
{
    memset(s, 0, sizeof *s);
    s->p16 = pool16; s->p32 = pool32;
#if REF2_LINE_STRUCT
    { unsigned i; for (i = 0; i < REF2_LINES; ++i) { s->ln[i].off = ref2_off[i]; s->ln[i].blen = ref2_blen[i]; } }
#endif
    s->lfo_ph[1] = 0x40000000u;
    ref2_clear(s);
}

REF2_API void ref2_apply(ref2_state_t *s, const ref2_coeffs_t *c)
{
    unsigned i;
    for (i = 0; i < REF2_LINES; ++i) {
        REF2_LN(s, i, a) = c->a[i]; REF2_LN(s, i, b) = c->b[i]; REF2_LN(s, i, dd) = c->dd[i];
        s->target[i] = c->target_q16[i];
    }
    s->hp_c = c->hp_c; s->gin_q12 = c->gin_q12; s->gout_q10 = c->gout_q10; s->width_q15 = c->width_q15;
    s->ap_g[0] = c->ap_g[0]; s->ap_g[1] = c->ap_g[1];
    if (c->sw_g && !s->sw_g)   /* swell APs were idle: drop stale content */
        memset(s->p16 + REF2_OSW0, 0, (REF2_SW0_LEN + REF2_SW1_LEN) * sizeof(int16_t));
    s->sw_g = c->sw_g; s->bloom = c->bloom;
    s->er_g = c->er_g;
    if (!s->er_g) s->er_l = s->er_r = 0;
    for (i = 0; i < 4u; ++i) s->er_t[i] = c->er_t[i];
    s->mod_depth_q8 = c->mod_depth_q8;
    s->lfo_inc[0] = c->lfo_inc[0]; s->lfo_inc[1] = c->lfo_inc[1];
    s->pre_target = c->pre_target; s->cap = c->cap; s->rt_slow = c->rt_slow;
    if (!s->configured) {   /* first apply: no slew */
        for (i = 0; i < REF2_LINES; ++i) s->base[i] = REF2_LN(s, i, d) = s->target[i];
        s->pre_d = s->pre_target;
        for (i = 0; i < 4u; ++i) s->er_d[i] = s->er_t[i];
        s->configured = 1u;
    }
}

REF2_API int ref2_idle(const ref2_state_t *s) { return (int)s->idle; }

/* parabolic sine, phase -> Q15 */
static int32_t refx_para(uint32_t ph)
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y = (x * (32768 - ax)) >> 13;
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}

/* ramps for the next REF2_SUB core samples: SIZE slew (1/32 sample per sample)
 * plus LFO, as a per-sample increment; pre-delay and early taps step 1 sample */
static void ref2_plan(ref2_state_t *s)
{
    unsigned i;
    int32_t m[4], step = 2048 * REF2_SUB;
    s->lfo_ph[0] += s->lfo_inc[0] * REF2_SUB;
    s->lfo_ph[1] += s->lfo_inc[1] * REF2_SUB;
    m[0] = refx_para(s->lfo_ph[0]); m[1] = refx_para(s->lfo_ph[1]);
    m[2] = refx_para(s->lfo_ph[0] + 0x40000000u); m[3] = refx_para(s->lfo_ph[1] + 0x40000000u);
    for (i = 0; i < REF2_LINES; ++i) {
        int32_t dlt = s->target[i] - s->base[i], lv, endp, hi = (ref2_blen[i] - 2) << 16;
        if (dlt > step) dlt = step; else if (dlt < -step) dlt = -step;
        s->base[i] += dlt;
        lv = m[i & 3u]; if (i & 4u) lv = -lv;
#if REF2_INT_READS
        if (i & 1u) lv = 0;
#endif
        endp = s->base[i] + ((s->mod_depth_q8 * lv) >> 7);
        if (endp < (2 << 16)) endp = 2 << 16;
        if (endp > hi) endp = hi;
        REF2_LN(s, i, inc) = (endp - REF2_LN(s, i, d)) >> 5;   /* REF2_SUB == 32 */
    }
    {
        uint32_t step = s->sync_mode ? 8u : 1u;
        if (s->sync_d < s->sync_target) {
            uint32_t diff = s->sync_target - s->sync_d;
            s->sync_d += (uint16_t)(diff < step ? diff : step);
        } else if (s->sync_d > s->sync_target) {
            uint32_t diff = s->sync_d - s->sync_target;
            s->sync_d -= (uint16_t)(diff < step ? diff : step);
        }
    }
    if (s->pre_d < s->pre_target) ++s->pre_d; else if (s->pre_d > s->pre_target) --s->pre_d;
    for (i = 0; i < 4u; ++i) { if (s->er_d[i] < s->er_t[i]) ++s->er_d[i]; else if (s->er_d[i] > s->er_t[i]) --s->er_d[i]; }
}

REF2_API void ref2_process(ref2_state_t *s, const int32_t *in, int32_t *out_l, int32_t *out_r, unsigned n)
{
    unsigned k, cores = n / 2u;
    if (s->idle) {
        for (k = 0; k < n; ++k) if (in[k]) break;
        if (k == n) { for (k = 0; k < n; ++k) out_l[k] = out_r[k] = 0; return; }
        s->idle = 0u; s->quiet = s->since_input = 0u;
    }
    s->loud = 0u;
    for (k = 0; k < n; k += 2u * REF2_SUB) {
        unsigned fr = n - k < 2u * REF2_SUB ? n - k : 2u * REF2_SUB;
        ref2_plan(s);
        REF2_CORE_CALL(ref2_core_run, s, in + k, out_l + k, out_r + k, fr);
    }
    /* Clean end. While input is present remember line-0 peak P. With no input:
     * (a) mean |line 0| <= 2 LSB for 0.1 s (rounding-noise floor reached), or
     * (b) the slowest band must have fallen from P to 1 LSB,
     * switches the loop quantisers to truncation (end mode), so the residue dies
     * to exact zero; then clear when every write stayed <= 1 LSB for 0.25 s,
     * or at the hard cap (1.6 RT + 0.3 s). */
    if (s->loud & 2u) {
        s->since_input = 0u; s->end = 0u; s->floor_blocks = 0u;
        s->peak_in = s->blk_peak;
    } else {
        s->since_input += cores;
        if (s->blk_peak > s->peak_in) s->peak_in = s->blk_peak;
    }
    if (!s->end && s->since_input) {
        uint32_t bits = 32u - (uint32_t)__builtin_clz((uint32_t)s->peak_in | 1u) - 2u;
        if (s->blk_abs <= 8u * cores) ++s->floor_blocks; else s->floor_blocks = 0u;
        if (s->floor_blocks * cores >= REF2_CORE_FS / 10u ||
            s->since_input >= s->rt_slow / 10u * bits + REF2_CORE_FS / 10u) s->end = 1u;
    }
    s->blk_peak = 0; s->blk_abs = 0u;
    if (s->loud & 1u) s->quiet = 0u; else s->quiet += cores;
    if (s->quiet >= REF2_CORE_FS / 4u || s->since_input >= s->cap) ref2_clear(s);
}
