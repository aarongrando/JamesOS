/* SPDX-License-Identifier: GPL-3.0-only */
/* The HD blend arithmetic (firmware/src/gfx_hd.h) against the original formula, which this file keeps as the
 * reference: per channel (a * (256 - t) + b * t) >> 8 with t widened to 0..256 (t += t >> 7).
 *  1. hd_mix: every 5-bit and every 6-bit channel pair for every coverage 0..255 (so each channel formula is exact),
 *     then 4 million colour pairs (random and the corner colours) at every coverage.
 *  2. hd_blend, the run blender and the prepared source (as they exist in gfx_hd.h): every coverage 0..255 over a
 *     set of canvas pixels and colours, against the reference on the byte-swapped canvas. */
#define DINGHY_HOST 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include "hd_look.h"

static uint32_t ref_channel(uint32_t a, uint32_t b, uint32_t t) { return (a * (256u - t) + b * t) >> 8; }
static uint16_t ref_mix(uint16_t a, uint16_t b, uint32_t t)
{
    uint32_t ar = a >> 11, ag = (a >> 5) & 63u, ab = a & 31u;
    uint32_t br = b >> 11, bg = (b >> 5) & 63u, bb = b & 31u;
    t += t >> 7;
    return (uint16_t)((ref_channel(ar, br, t) << 11) | (ref_channel(ag, bg, t) << 5) | ref_channel(ab, bb, t));
}
static uint32_t rng = 0x12345678u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

int main(int argc, char **argv)
{
    uint32_t bad = 0, pairs = 0;
    (void)argc; (void)argv;
    /* 1a. every channel pair, every coverage: the 6-bit range covers the 5-bit one; take both as pixels */
    for (uint32_t a = 0; a < 64u; ++a)
        for (uint32_t b = 0; b < 64u; ++b)
            for (uint32_t t = 0; t < 256u; ++t) {
                uint16_t pa = (uint16_t)(a << 5), pb = (uint16_t)(b << 5);          /* green */
                uint16_t qa = (uint16_t)((a & 31u) << 11 | (a & 31u)), qb = (uint16_t)((b & 31u) << 11 | (b & 31u)); /* red + blue */
                bad += hd_mix(pa, pb, t) != ref_mix(pa, pb, t);
                bad += hd_mix(qa, qb, t) != ref_mix(qa, qb, t);
                pairs += 2u;
            }
    HD_CHECK(!bad, "%u of %u channel mixes differ from the reference", bad, pairs);
    /* 1b. whole colours */
    bad = 0; pairs = 0;
    for (uint32_t i = 0; i < 16384u; ++i) {
        uint16_t a = i < 4u ? (uint16_t)(i & 1u ? 0xFFFFu : 0u) : (uint16_t)rnd();
        uint16_t b = i < 4u ? (uint16_t)(i & 2u ? 0xFFFFu : 0u) : (uint16_t)rnd();
        for (uint32_t t = 0; t < 256u; ++t) { bad += hd_mix(a, b, t) != ref_mix(a, b, t); ++pairs; }
    }
    HD_CHECK(!bad, "%u of %u colour mixes differ from the reference", bad, pairs);
    /* 2. the canvas blend */
    cv_begin(8, 2, 0u);
    bad = 0; pairs = 0;
    for (uint32_t i = 0; i < 2048u; ++i) {
        uint16_t bg = i < 4u ? (uint16_t)(i & 1u ? 0xFFFFu : 0u) : (uint16_t)rnd(), c = i < 4u ? (uint16_t)(i & 2u ? 0xFFFFu : 0u) : (uint16_t)rnd();
        for (uint32_t a = 0; a < 256u; ++a) {
            uint16_t want = !a ? bg : a >= 255u ? c : ref_mix(bg, c, a);
            cv_px[5] = swap16(bg);
            hd_blend(5, 0, c, a);
            bad += cv_px[5] != swap16(want);
            cv_px[5] = swap16(bg);
            hd__px(5, 0, c, (int32_t)a);
            bad += cv_px[5] != swap16(want);
            ++pairs;
        }
    }
    HD_CHECK(!bad, "%u of %u canvas blends differ from the reference", bad, pairs * 2u);
    return hd_finish("HD blend arithmetic");
}
