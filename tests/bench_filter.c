/* Host benchmark of the track filter: ns per sample for the frozen per-sample reference (what shipped before the
 * block API), the live per-sample entry points, and the live block API, on blocks of 256 samples like the engine.
 * A tool, not a test: it asserts nothing about timings (build and run it with tests/bench_filter.py). The host CPU is
 * not the target CPU; tests/test_filter_equiv.c proves the three produce identical output. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../firmware/src/instrument_filter.c"
#include "filter_ref_impl.h"

#define BLK 256
typedef struct { const char *name; uint32_t control, alt, res, key, note; int glide; } path_t;
static int32_t in_noise[BLK * 64], in_sine[BLK * 64];
static volatile int32_t sink;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

/* mode 0: reference per-sample, 1: live per-sample, 2: live block */
static double run(const path_t *p, int mode, const int32_t *in, int blocks)
{
    br_filter_t a; ref_filter_t b; int32_t row[BLK]; double best = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        double t0;
        br_filter_init(&a); ref_filter_init(&b);
        /* warm up into the steady state (the mix fade takes about 100 samples) */
        for (int w = 0; w < 8; ++w) {
            memcpy(row, in + w * BLK, sizeof row);
#ifdef BR_FILTER_HAS_BLOCK
            br_filter_block_controls(&a, row, BLK, p->control, p->res, p->key, p->note);
#endif
            for (int i = 0; i < BLK; ++i) ref_filter_process_controls(&b, in[w * BLK + i], p->control, p->res, p->key, p->note);
        }
        t0 = now();
        for (int blk = 0; blk < blocks; ++blk) {
            const int32_t *src = in + (blk & 63) * BLK;
            uint32_t c = p->glide && (blk & 1) ? p->alt : p->control;
            if (mode == 2) {
#ifdef BR_FILTER_HAS_BLOCK
                memcpy(row, src, sizeof row);
                if (!p->res && !p->key) br_filter_block(&a, row, BLK, c, 0u);
                else br_filter_block_controls(&a, row, BLK, c, p->res, p->key, p->note);
#endif
            } else if (mode == 1) {
                if (!p->res && !p->key) for (int i = 0; i < BLK; ++i) row[i] = br_filter_process(&a, src[i], c);
                else for (int i = 0; i < BLK; ++i) row[i] = br_filter_process_controls(&a, src[i], c, p->res, p->key, p->note);
            } else {
                if (!p->res && !p->key) for (int i = 0; i < BLK; ++i) row[i] = ref_filter_process(&b, src[i], c);
                else for (int i = 0; i < BLK; ++i) row[i] = ref_filter_process_controls(&b, src[i], c, p->res, p->key, p->note);
            }
            sink += row[blk & 255];
        }
        double ns = (now() - t0) / ((double)blocks * BLK);
        if (ns < best) best = ns;
    }
    return best;
}

int main(int argc, char **argv)
{
    int blocks = argc > 1 ? atoi(argv[1]) : 20000;
    static const path_t paths[] = {
        { "bypass (64)",                64, 64,  0, 0, 60, 0 },
        { "ladder res 0 (30)",          30, 30,  0, 0, 60, 0 },
        { "ladder res 64",              30, 30, 64, 0, 60, 0 },
        { "ladder res 118",             30, 30,118, 0, 60, 0 },
        { "ladder res 64 (control 10)", 10, 10, 64, 0, 60, 0 },
        { "ladder res 64 (control 20)", 20, 20, 64, 0, 60, 0 },
        { "ladder res 64 (control 45)", 45, 45, 64, 0, 60, 0 },
        { "ladder res 0 gliding",       20, 44,  0, 0, 60, 1 },
        { "ladder res 64 gliding",      20, 44, 64, 0, 60, 1 },
        { "ladder res 0 key-tracked",   30, 30,  0,64, 72, 0 },
        { "high-pass res 0 (90)",       90, 90,  0, 0, 60, 0 },
        { "high-pass res 100",          90, 90,100, 0, 60, 0 },
        { "high-pass res 0 gliding",    80,110,  0, 0, 60, 1 },
    };
    uint32_t seed = 1;
    for (int i = 0; i < BLK * 64; ++i) {
        seed = seed * 1664525u + 1013904223u;
        in_noise[i] = (int32_t)(((int64_t)(int32_t)seed * 16384) >> 31);
        in_sine[i] = (int32_t)(sin(i * 0.05) * 16384.0);
    }
    printf("%-26s %10s %10s %10s %8s   (ns per sample, noise at half scale; sine in brackets for the block API)\n",
           "path", "reference", "live/smp", "live/blk", "speedup");
    for (unsigned k = 0; k < sizeof paths / sizeof paths[0]; ++k) {
        double r = run(&paths[k], 0, in_noise, blocks), l = run(&paths[k], 1, in_noise, blocks);
        double b = run(&paths[k], 2, in_noise, blocks), bs = run(&paths[k], 2, in_sine, blocks);
        double rs = run(&paths[k], 0, in_sine, blocks);
        printf("%-26s %10.2f %10.2f %10.2f %7.2fx   (sine: %.2f -> %.2f)\n", paths[k].name, r, l, b, b > 0 ? r / b : 0.0, rs, bs);
    }
    return 0;
}
