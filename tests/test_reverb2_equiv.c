/* The live shared reverb (firmware/src/instrument_reverb2*.c|h) must stay bit-for-bit identical to the implementation it
 * replaced, kept frozen in tests/reverb2_ref_impl.h as ref2_*: same output sample for sample and the same state (every field
 * of the state struct and every word of both pools) after every block. All three engines; sizes, decays and the user overrides
 * including tone, modulation, low cut, width and pre-delay; the long synced pre-delay in both of its modes; parameter changes
 * in mid-stream (design and apply again, with the SIZE slew running); silence into the idle state and the clean end and back;
 * input from nothing to dense noise, clicks, chords, with loud (clipped) input. No timings are asserted. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#define RV2_API static
#define RV2_RAM __attribute__((noinline))
#define RV2_CORE_CALL(fn, ...) ({ __typeof__(&(fn)) volatile rv2_core_fn_ = &(fn); rv2_core_fn_(__VA_ARGS__); })
#include "../firmware/src/instrument_reverb2.c"
#define REF2_API static
#define REF2_RAM __attribute__((noinline))
#define REF2_CORE_CALL(fn, ...) ({ __typeof__(&(fn)) volatile ref2_core_fn_ = &(fn); ref2_core_fn_(__VA_ARGS__); })
#include "reverb2_ref_impl.h"

/* The live line struct may hold a pointer (4 bytes on the device, 8 here), so the structs are compared in parts: the members before
 * the pools' pointers, the per-line array field by field, and everything from `base` on. */
_Static_assert(sizeof(rv2_state_t) - offsetof(rv2_state_t, base) == sizeof(ref2_state_t) - offsetof(ref2_state_t, base), "state tail changed: update the comparison");
_Static_assert(offsetof(rv2_state_t, p32) - offsetof(rv2_state_t, sync_pos) == offsetof(ref2_state_t, p32) - offsetof(ref2_state_t, sync_pos), "state head changed: update the comparison");

static uint32_t g_rng = 0xC0FFEEu;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint32_t rnd_n(uint32_t n) { return rnd() % n; }
static uint64_t g_blocks, g_frames;

static void fail(const char *what, uint64_t at)
{
    fprintf(stderr, "REVERB2 EQUIV FAIL: %s after %llu blocks\n", what, (unsigned long long)at);
    exit(1);
}

typedef struct {
    rv2_state_t a; ref2_state_t b;
    int16_t pa16[RV2_POOL16_LEN], pb16[REF2_POOL16_LEN]; int32_t pa32[RV2_POOL32_LEN], pb32[REF2_POOL32_LEN];
    int16_t sa[RV2_SYNC_PRE_LEN], sb[REF2_SYNC_PRE_LEN];
} pair_t;
static pair_t P;

static void init_pair(void)
{
    memset(&P, 0, sizeof P);
    rv2_init(&P.a, P.pa16, P.pa32); ref2_init(&P.b, P.pb16, P.pb32);
    P.a.sync_pre = P.sa; P.b.sync_pre = P.sb;
}

static int same_lines(void)
{
    for (int i = 0; i < RV2_LINES; ++i) {
        const rv2_line_t *x = &P.a.ln[i]; const ref2_line_t *y = &P.b.ln[i];
        if (x->d != y->d || x->inc != y->inc || x->s != y->s || x->e != y->e || x->es != y->es || x->yap != y->yap ||
            x->a != y->a || x->b != y->b || x->dd != y->dd || x->wpos != y->wpos || x->blen != y->blen ||
#if RV2_LINE_PTR
            x->buf - P.a.p16 != (ptrdiff_t)y->off) return 0;
#else
            x->off != y->off) return 0;
#endif
    }
    return 1;
}
static int same_state(void)
{
    const unsigned char *x = (const unsigned char *)&P.a, *y = (const unsigned char *)&P.b;
    int ok = same_lines();
    ok = ok && !memcmp(x + offsetof(rv2_state_t, sync_pos), y + offsetof(ref2_state_t, sync_pos),
                       offsetof(rv2_state_t, p32) - offsetof(rv2_state_t, sync_pos));
    ok = ok && !memcmp(x + offsetof(rv2_state_t, base), y + offsetof(ref2_state_t, base), sizeof(rv2_state_t) - offsetof(rv2_state_t, base));
    if (P.a.p16 != P.pa16 || P.a.p32 != P.pa32 || P.a.sync_pre != P.sa) ok = 0;
    return ok && !memcmp(P.pa16, P.pb16, sizeof P.pa16) && !memcmp(P.pa32, P.pb32, sizeof P.pa32) && !memcmp(P.sa, P.sb, sizeof P.sa);
}

static void design_both(unsigned type, unsigned size, unsigned decay, const rv2_overrides_t *ov)
{
    rv2_coeffs_t ca; ref2_coeffs_t cb;
    ref2_overrides_t ob;
    if (ov) { ob.pre = ov->pre; ob.tone = ov->tone; ob.mod = ov->mod; ob.lowcut = ov->lowcut; ob.width = ov->width; }
    rv2_design(type, size, decay, ov, &ca);
    ref2_design(type, size, decay, ov ? &ob : 0, &cb);
    if (memcmp(&ca, &cb, sizeof ca)) fail("design differs", g_blocks);
    rv2_apply(&P.a, &ca); ref2_apply(&P.b, &cb);
}

/* input kinds: 0 silence, 1 dense noise, 2 click train, 3 saw chord, 4 loud clipped noise, 5 sparse noise */
static void fill(int32_t *in, unsigned n, int kind, uint32_t *ph)
{
    for (unsigned i = 0; i < n; ++i) {
        int32_t x = 0;
        switch (kind) {
        case 1: x = (int32_t)(rnd() >> 15) - 65536; break;
        case 2: x = rnd_n(97) == 0 ? 200000 : 0; break;
        case 3: { static const uint32_t inc[3] = {0x00C80000u, 0x00FC0000u, 0x012C0000u}; int32_t s = 0;
                  for (int k = 0; k < 3; ++k) { ph[k] += inc[k]; s += (int32_t)ph[k] >> 14; } x = s / 4; } break;
        case 4: x = (int32_t)(rnd() >> 9) - 4000000; break;
        case 5: x = rnd_n(9) == 0 ? (int32_t)(rnd() >> 20) - 2048 : 0; break;
        default: break;
        }
        in[i] = x;
    }
}

static void run_block(unsigned n, int kind, uint32_t *ph)
{
    int32_t in[256], la[256], ra[256], lb[256], rb[256];
    fill(in, n, kind, ph);
    rv2_process(&P.a, in, la, ra, n); ref2_process(&P.b, in, lb, rb, n);
    ++g_blocks; g_frames += n;
    if (memcmp(la, lb, n * 4u) || memcmp(ra, rb, n * 4u)) fail("output differs", g_blocks);
    if (!same_state()) fail("state differs", g_blocks);
    if (rv2_idle(&P.a) != ref2_idle(&P.b)) fail("idle differs", g_blocks);
}

static void random_overrides(rv2_overrides_t *o)
{
    o->pre = rnd_n(3) ? RV2_DERIVE : rnd_n(128); o->tone = rnd_n(3) ? RV2_DERIVE : rnd_n(128);
    o->mod = rnd_n(3) ? RV2_DERIVE : rnd_n(128); o->lowcut = rnd_n(3) ? RV2_DERIVE : rnd_n(128);
    o->width = rnd_n(3) ? RV2_DERIVE : rnd_n(128);
}

static void scenario(uint32_t segments)
{
    uint32_t ph[3] = {0, 0, 0}, seg;
    unsigned type = 0, size = 64, decay = 64;
    rv2_overrides_t ov; random_overrides(&ov);
    init_pair();
    design_both(type, size, decay, &ov);
    for (seg = 0; seg < segments; ++seg) {
        static const unsigned types[3] = {0, 1, 3};
        unsigned blocks = 4u + rnd_n(rnd_n(4) ? 60u : 400u), kind = rnd_n(6);
        if (rnd_n(2)) {            /* change the settings in mid-stream, sometimes only SIZE (the slew) */
            if (rnd_n(2)) type = types[rnd_n(3)];
            size = rnd_n(128); if (rnd_n(2)) decay = rnd_n(128);
            if (rnd_n(2)) random_overrides(&ov);
            design_both(type, size, decay, rnd_n(4) ? &ov : 0);
        }
        if (rnd_n(5) == 0) {       /* synced pre-delay: the extra line, plain (mode 0) or compact (mode 1) */
            unsigned mode = rnd_n(2), d = rnd_n(mode ? 3000u : (unsigned)RV2_SYNC_PRE_LEN - 1u), tg = rnd_n(mode ? 3000u : (unsigned)RV2_SYNC_PRE_LEN - 1u);
            if (mode != P.a.sync_mode) { /* what br_gr_prepare does on a mode change */
                memset(P.sa, 0, sizeof P.sa); memset(P.sb, 0, sizeof P.sb);
                memset(P.a.sync_hist, 0, sizeof P.a.sync_hist); memset(P.b.sync_hist, 0, sizeof P.b.sync_hist);
                P.a.sync_pos = P.a.sync_phase = P.a.sync_pending = 0u; P.b.sync_pos = P.b.sync_phase = P.b.sync_pending = 0u;
            }
            P.a.sync_mode = P.b.sync_mode = (uint8_t)mode;
            P.a.sync_d = P.b.sync_d = (uint16_t)d; P.a.sync_target = P.b.sync_target = (uint16_t)tg;
        }
        for (unsigned b = 0; b < blocks; ++b) {
            unsigned n = rnd_n(8) == 0 ? 2u * (1u + rnd_n(127u)) : 256u;
            run_block(n, rnd_n(10) == 0 ? (int)rnd_n(6) : (int)kind, ph);
        }
        if (rnd_n(4) == 0) {       /* long silence: the clean end, the clear, the idle state, and back */
            unsigned silent = 80u + rnd_n(1500u);
            for (unsigned b = 0; b < silent; ++b) run_block(256u, 0, ph);
        }
    }
}

int main(int argc, char **argv)
{
    uint32_t scale = argc > 1 ? (uint32_t)atoi(argv[1]) : 1u;
    g_rng = 0xC0FFEEu; scenario(60u * scale);
    g_rng = 0x1234ABCu; scenario(60u * scale);
    printf("reverb2 equivalence: %llu blocks (%llu frames), output and the whole state (struct and both pools) identical to the frozen reference\n",
           (unsigned long long)g_blocks, (unsigned long long)g_frames);
    return 0;
}
