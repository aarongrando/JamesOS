/* SPDX-License-Identifier: GPL-3.0-only
 * Host probe (a tool, not a test): what does the first 256-frame block of a Plaits note start cost, per model,
 * (warm) while the engine is still rendering its release, (stopped) after a stop of 3 s with nothing in between,
 * (evicted) after a stop plus 8 MB of other memory traffic (what the flash cache sees on the unit when other
 * tracks run). Median of 25 runs, nanoseconds per block. */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>
#include <algorithm>
#include "plaits_c_api.h"
static uint16_t q15(uint32_t v) { return (uint16_t)((v * 32767u + 63u) / 127u); }
static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static void rend(void *st, uint32_t m, uint32_t frames) {
    int16_t o[256];
    for (uint32_t d = 0; d < frames; d += 256) plaits_render(st, m, 60 * 128, q15(64), q15(64), q15(64), 0, o, 256);
}
static double first_block(void *st, uint32_t m) {
    int16_t o[256];
    plaits_note_on(st, m, 60 * 128, q15(64), q15(64), q15(64), 100);
    double t0 = now(); plaits_render(st, m, 60 * 128, q15(64), q15(64), q15(64), 0, o, 256); return now() - t0;
}
int main() {
    alignas(8) static uint8_t st[PLAITS_STATE_BYTES];
    static std::vector<uint8_t> junk(8u << 20);
    printf("model,warm_us,stopped_us,evicted_us\n");
    for (uint32_t m = 0; m < 24; ++m) {
        std::vector<double> w, s, e;
        for (int r = 0; r < 25; ++r) {
            for (int mode = 0; mode < 3; ++mode) {
                plaits_state_init(st, 0x3456789u);
                plaits_note_on(st, m, 60 * 128, q15(64), q15(64), q15(64), 100);
                rend(st, m, 22050); plaits_note_off(st, 128);
                rend(st, m, mode == 0 ? 4410 : 3 * 44100);   /* warm: 100 ms; else 3 s of rendering = quiet reached */
                if (mode == 2) { memset(junk.data(), r, junk.size()); }
                double t = first_block(st, m);
                (mode == 0 ? w : mode == 1 ? s : e).push_back(t);
            }
        }
        auto med = [](std::vector<double> &v) { std::sort(v.begin(), v.end()); return v[v.size() / 2] / 1000.0; };
        printf("%u,%.1f,%.1f,%.1f\n", m, med(w), med(s), med(e));
    }
}
