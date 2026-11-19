/* Host benchmark of the shared reverb: ns per 256-frame block for the frozen reference and the live code, per engine, with a
 * dense input (noise at +-65,535, continuous: the loop never idles). A tool, not a test; asserts no timings.
 * The host CPU is not the target; use the device assembly counts (see the commit messages) for the real cost. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define RV2_API static
#define RV2_RAM __attribute__((noinline))
#define RV2_CORE_CALL(fn, ...) ({ __typeof__(&(fn)) volatile rv2_core_fn_ = &(fn); rv2_core_fn_(__VA_ARGS__); })
#include "../firmware/src/instrument_reverb2.c"
#define REF2_API static
#define REF2_RAM __attribute__((noinline))
#define REF2_CORE_CALL(fn, ...) ({ __typeof__(&(fn)) volatile ref2_core_fn_ = &(fn); ref2_core_fn_(__VA_ARGS__); })
#include "reverb2_ref_impl.h"

static volatile int32_t sink;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static int16_t pa16[RV2_POOL16_LEN], pb16[REF2_POOL16_LEN], sa[RV2_SYNC_PRE_LEN], sb[REF2_SYNC_PRE_LEN];
static int32_t pa32[RV2_POOL32_LEN], pb32[REF2_POOL32_LEN];
static rv2_state_t A; static ref2_state_t B;

int main(int argc, char **argv)
{
    int blocks = argc > 1 ? atoi(argv[1]) : 4000;
    static const struct { const char *name; unsigned type, size, decay; } cfg[] = {
        { "ROOM  (size 64, decay 64)", 0, 64, 64 }, { "HALL  (size 64, decay 64)", 1, 64, 64 }, { "SPACE (size 64, decay 64)", 3, 64, 64 },
        { "ROOM  (size 127, decay 127)", 0, 127, 127 }, { "SPACE (size 127, decay 127)", 3, 127, 127 },
    };
    static int32_t in[256], l[256], r[256];
    uint32_t seed = 5;
    for (int i = 0; i < 256; ++i) { seed = seed * 1664525u + 1013904223u; in[i] = (int32_t)(seed >> 15) - 65536; }
    printf("%-30s %12s %12s %9s   (ns per 256-frame block, dense noise)\n", "engine", "reference", "live", "speedup");
    for (unsigned k = 0; k < sizeof cfg / sizeof cfg[0]; ++k) {
        rv2_coeffs_t ca; ref2_coeffs_t cb; double bestA = 1e30, bestB = 1e30;
        rv2_design(cfg[k].type, cfg[k].size, cfg[k].decay, 0, &ca); ref2_design(cfg[k].type, cfg[k].size, cfg[k].decay, 0, &cb);
        for (int rep = 0; rep < 5; ++rep) {
            double t0, ns;
            memset(pa16, 0, sizeof pa16); memset(pa32, 0, sizeof pa32); memset(sa, 0, sizeof sa);
            rv2_init(&A, pa16, pa32); A.sync_pre = sa; rv2_apply(&A, &ca);
            for (int b = 0; b < 50; ++b) rv2_process(&A, in, l, r, 256);
            t0 = now(); for (int b = 0; b < blocks; ++b) { rv2_process(&A, in, l, r, 256); sink += l[b & 255]; }
            ns = (now() - t0) / blocks; if (ns < bestA) bestA = ns;
            memset(pb16, 0, sizeof pb16); memset(pb32, 0, sizeof pb32); memset(sb, 0, sizeof sb);
            ref2_init(&B, pb16, pb32); B.sync_pre = sb; ref2_apply(&B, &cb);
            for (int b = 0; b < 50; ++b) ref2_process(&B, in, l, r, 256);
            t0 = now(); for (int b = 0; b < blocks; ++b) { ref2_process(&B, in, l, r, 256); sink += l[b & 255]; }
            ns = (now() - t0) / blocks; if (ns < bestB) bestB = ns;
        }
        printf("%-30s %12.0f %12.0f %8.2fx\n", cfg[k].name, bestB, bestA, bestB / bestA);
    }
    return 0;
}
