/* SPDX-License-Identifier: GPL-3.0-only
 * Host probe (a tool, not a test): how much does the Plaits engine's warm-keep window matter?
 * Per model, parameter set, note pair and window W: play a note, release it, keep rendering W output frames
 * counted from the release (what plaits_quiet does), stop rendering, then play the next note and compare its
 * first 2 s with the reference where the engine kept rendering for the current window. Prints one CSV line per
 * case; tests/probe_plaits_warm.py summarises. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cmath>
#include "plaits_c_api.h"

static const uint8_t factory[24][3] = {
    {48,76,64},{64,64,64},{12,76,64},{12,76,64},{12,76,64},{32,64,64},{48,64,64},{64,64,64},
    {32,64,64},{64,76,64},{48,64,64},{64,64,64},{64,76,64},{32,64,64},{48,64,64},{32,64,64},
    {32,64,127},{32,64,48},{32,64,64},{48,76,76},{64,76,76},{24,48,64},{80,64,52},{80,100,24}};
static uint16_t q15(uint32_t v) { return (uint16_t)((v * 32767u + 63u) / 127u); }

struct Params { uint16_t timbre, morph, harm, aux; };
static const uint32_t kRate = 44100u;
static const uint32_t kNote1 = kRate / 2u;       /* 0.5 s of the first note */
static const uint32_t kNext = 2u * kRate;        /* the next note's first 2 s */

static void render(void *st, uint32_t model, int32_t pitch, const Params &p, uint32_t frames, int16_t *out)
{
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done < 256u ? frames - done : 256u;
        int16_t tmp[256];
        plaits_render(st, model, pitch, p.timbre, p.morph, p.harm, p.aux, out ? out + done : tmp, n);
        done += n;
    }
}

/* the character of a difference when the waveforms are not sample-aligned: the 10 ms RMS envelope of both, the
 * level ratio over the 2 s and the ratio over the first 30 ms (a missing or soft attack shows there) */
static void character(const std::vector<int16_t> &a, const std::vector<int16_t> &b, double &env_diff, double &level, double &attack)
{
    const uint32_t win = kRate / 100u;
    double esum = 0, ta = 0, tb = 0, aa = 0, ab = 0;
    uint32_t nw = kNext / win;
    for (uint32_t w = 0; w < nw; ++w) {
        double sa = 0, sb = 0;
        for (uint32_t i = 0; i < win; ++i) { double x = a[w * win + i], y = b[w * win + i]; sa += x * x; sb += y * y; }
        double ra = std::sqrt(sa / win), rb = std::sqrt(sb / win);
        esum += (ra - rb) * (ra - rb); ta += sa; tb += sb;
        if (w < 3) { aa += sa; ab += sb; }
    }
    env_diff = std::sqrt(esum / nw);
    level = ta > 0 ? std::sqrt(tb / ta) : (tb > 0 ? 99.0 : 1.0);
    attack = aa > 0 ? std::sqrt(ab / aa) : (ab > 0 ? 99.0 : 1.0);
}

/* returns the next note's output */
static void run_case(uint32_t model, const Params &p, uint32_t note1, uint32_t note2, uint32_t window,
                     std::vector<int16_t> &next)
{
    alignas(8) static uint8_t st[PLAITS_STATE_BYTES];
    plaits_state_init(st, 0x3456789u);
    plaits_note_on(st, model, (int32_t)note1 * 128, p.timbre, p.morph, p.harm, 100);
    render(st, model, (int32_t)note1 * 128, p, kNote1, nullptr);
    plaits_note_off(st, 128);
    render(st, model, (int32_t)note1 * 128, p, window, nullptr);      /* the warm window, counted from the release */
    /* (not rendering: the pause) */
    plaits_note_on(st, model, (int32_t)note2 * 128, p.timbre, p.morph, p.harm, 100);
    next.assign(kNext, 0);
    render(st, model, (int32_t)note2 * 128, p, kNext, next.data());
}

int main(int argc, char **argv)
{
    uint32_t only_model = argc > 1 ? (uint32_t)atoi(argv[1]) : 99u;
    static const uint32_t windows_ms[] = {0, 16, 32, 64, 128, 256, 512, 1000, 2000, 3000, 12000};
    static const uint32_t notes[3] = {36, 60, 84};
    printf("model,set,note1,note2,window_ms,max_lsb,rms_lsb,ref_ms,floor_max,floor_rms,env_diff,level_ratio,attack_ratio,floor_env,floor_level,floor_attack\n");
    for (uint32_t model = 0; model < 24u; ++model) {
        if (only_model != 99u && model != only_model) continue;
        uint32_t ref_frames = (model >= 21u && model <= 23u) ? (3u * kRate) / 4u : 3u * kRate;  /* today's window */
        Params sets[3];
        sets[0] = {q15(factory[model][1]), q15(factory[model][2]), q15(factory[model][0]), 0};
        sets[1] = {q15(16), q15(16), q15(112), q15(64)};
        sets[2] = {q15(112), q15(112), q15(16), 0};
        for (uint32_t set = 0; set < 3; ++set)
            for (uint32_t ni = 0; ni < 3; ++ni)
                for (uint32_t same = 0; same < 2; ++same) {
                    uint32_t n1 = notes[ni], n2 = same ? n1 : (n1 + 7u);
                    std::vector<int16_t> ref, inf, got;
                    run_case(model, sets[set], n1, n2, ref_frames, ref);
                    run_case(model, sets[set], n1, n2, 12u * kRate, inf);
                    double fmax = 0, fsum = 0;
                    for (uint32_t i = 0; i < kNext; ++i) { double d = std::fabs((double)ref[i] - inf[i]); if (d > fmax) fmax = d; fsum += d * d; }
                    double frms = std::sqrt(fsum / kNext), fenv, flev, fatt;
                    character(ref, inf, fenv, flev, fatt);
                    for (uint32_t wi = 0; wi < sizeof windows_ms / sizeof windows_ms[0]; ++wi) {
                        run_case(model, sets[set], n1, n2, (uint32_t)((uint64_t)windows_ms[wi] * kRate / 1000u), got);
                        double mx = 0, sum = 0;
                        for (uint32_t i = 0; i < kNext; ++i) { double d = std::fabs((double)ref[i] - got[i]); if (d > mx) mx = d; sum += d * d; }
                        double env, lev, att;
                        character(ref, got, env, lev, att);
                        printf("%u,%u,%u,%u,%u,%.0f,%.3f,%u,%.0f,%.3f,%.3f,%.4f,%.4f,%.3f,%.4f,%.4f\n", model, set, n1, n2, windows_ms[wi], mx,
                               std::sqrt(sum / kNext), ref_frames * 1000u / kRate, fmax, frms, env, lev, att, fenv, flev, fatt);
                    }
                }
        fflush(stdout);
    }
    return 0;
}
