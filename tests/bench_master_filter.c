/* Host benchmark of the master low-pass: ns per stereo frame through br_master_fx_process_range (the whole master
 * frame: filter, shaping, the rest at their neutral settings), with the block pre-pass off (the per-frame path, as it was)
 * and on. A tool, not a test; asserts nothing about timings. Build and run with tests/bench_master_filter.py. */
#define DINGHY_HOST 1
#define BR_MASTER_LADDER_BLOCK_TEST_SWITCH 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static volatile int32_t sink;

static double run(uint32_t filter, uint32_t res, int enable, int slice16, int blocks, int mono)
{
    static int32_t in[256 * 2], st[256 * 2];
    double best = 1e30; uint32_t seed = 7u;
    br_app_init(); fm1_ms = 1000u;
    memset(&br_gf, 0, sizeof br_gf);
    br_master.fx.env_depth = 0; br_master.revision = 0u;
    br_master.fx.filter = filter; br_master.fx.resonance = res;
    br_master.fx.heat_type = BR_HEAT_BIT; br_master.fx.heat_amount = 0u;
    br_master_fx_init();
    br_master_ladder_block_enable = (uint32_t)enable;
    for (int i = 0; i < 512; ++i) { seed = seed * 1664525u + 1013904223u; in[i] = (int32_t)(((int64_t)(int32_t)seed * 16384) >> 31); if (mono && (i & 1)) in[i] = in[i - 1]; }
    for (int rep = 0; rep < 5; ++rep) {
        for (int w = 0; w < 100; ++w) { memcpy(st, in, sizeof st); br_master_fx_begin_block(256u); br_master_fx_process_range(st, 0u, 256u); }
        double t0 = now();
        for (int b = 0; b < blocks; ++b) {
            memcpy(st, in, sizeof st);
            br_master_fx_begin_block(256u);
            if (slice16) for (uint32_t f = 0; f < 256u; f += 16u) br_master_fx_process_range(st, f, f + 16u);
            else br_master_fx_process_range(st, 0u, 256u);
            sink += st[b & 255];
        }
        double ns = (now() - t0) / ((double)blocks * 256.0);
        if (ns < best) best = ns;
    }
    return best;
}

int main(int argc, char **argv)
{
    int blocks = argc > 1 ? atoi(argv[1]) : 4000;
    static const struct { const char *name; uint32_t filter, res; } p[] = {
        { "master LP res 0 (filter 30)", 30, 0 }, { "master LP res 64", 30, 64 }, { "master LP res 118", 30, 118 },
    };
    printf("%-28s %-22s %10s %10s %8s  (ns per stereo frame)\n", "path", "range", "per-frame", "pre-pass", "speedup");
    for (int m = 0; m < 2; ++m)
    for (int k = 0; k < 3; ++k)
        for (int s = 1; s >= 0; --s) {
            double a = run(p[k].filter, p[k].res, 0, s, blocks, m), b = run(p[k].filter, p[k].res, 1, s, blocks, m);
            printf("%-28s %-22s %10.2f %10.2f %7.2fx\n", m ? (k == 0 ? "MONO LP res 0" : k == 1 ? "MONO LP res 64" : "MONO LP res 118") : p[k].name,
                   s ? "16-frame slices (stream)" : "whole 256-frame block", a, b, a / b);
        }
    return 0;
}
