/* Probe: does the kit's shared part envelope fade drums that are still ringing, and how much does its first-hit attack ramp take off
 * the first hit? Compare the real behaviour with the envelope held open (BR_KIT_ENV_HOLD_PROBE: a note-off does not release it) or
 * already open at the first hit. Prints tables; asserts nothing. A tool, not a test. Build: tests/probe_kit_release.py */
#define DINGHY_HOST 1
#define BR_KIT_ENV_HOLD_PROBE 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define MAXB 560u
static int32_t g_out[2][MAXB * BR_BLOCK * 2u];
typedef struct { uint32_t block; int on; uint32_t lane; } ev_t;

static int g_attack = -1;
static void set_sound(uint32_t decay, uint32_t gate, int tune, uint32_t voice_mask)
{
    br_part[BR_DRUM_PART].revision++;
    if (g_attack >= 0) br_part[BR_DRUM_PART].sound.attack = (uint32_t)g_attack;
    br_part[BR_DRUM_PART].sound.decay = decay; br_part[BR_DRUM_PART].sound.gate = gate; br_part[BR_DRUM_PART].sound.tune = tune;
    if (voice_mask != 0xFFFFFFFFu) br_part[BR_DRUM_PART].sound.drum_voice_mask = voice_mask;
    RING_PUBLISH(); br_part[BR_DRUM_PART].revision++;
}
/* live events; hold: the envelope is not released by a note-off; prime: the envelope is open before the first hit. */
static void render(uint32_t preset, uint32_t decay, uint32_t gate, int tune, uint32_t voice_mask, const ev_t *ev, unsigned nev, unsigned blocks, int hold, int prime, int32_t *out)
{
    br_app_init(); fm1_ms = 2000u;
    br_load_preset(BR_DRUM_PART, preset);
    set_sound(decay, gate, tune, voice_mask);
    br_kit_env_hold_probe = (uint32_t)hold;
    if (prime) { br_env_t *e = &br_env[BR_DRUM_PART]; br_sound_t sd = br_part[BR_DRUM_PART].sound; br_env_start(BR_DRUM_PART, 36u, 100u, &sd, 0u); e->stage = BR_ENV_SUSTAIN; e->amp = 32767u; e->pos = 32767u << 16; }
    for (unsigned b = 0; b < blocks; ++b) {
        for (unsigned k = 0; k < nev; ++k) if (ev[k].block == b) {
            uint32_t note = br_drum_lane_note(ev[k].lane);
            if (ev[k].on) br_engine_note_on(BR_DRUM_PART, note, 120u); else br_engine_note_off(BR_DRUM_PART, note);
        }
        memset(&out[b * BR_BLOCK * 2u], 0, BR_BLOCK * 2u * sizeof(int32_t));
        assert(app_audio(&out[b * BR_BLOCK * 2u], BR_BLOCK) == FM1_AUDIO_RENDER_OK);
    }
}

static double win_rms(const int32_t *x, unsigned from_ms, unsigned len_ms)
{
    unsigned a = from_ms * 44100u / 1000u, n = len_ms * 44100u / 1000u; double s = 0;
    for (unsigned i = 0; i < n; ++i) { double v = x[2u * (a + i)]; s += v * v; }
    return sqrt(s / n);
}
static double db(double a, double b) { return (a < 0.5 || b < 0.5) ? (a < 0.5 && b < 0.5 ? 0.0 : (a < 0.5 ? -99.0 : 99.0)) : 20.0 * log10(a / b); }
static unsigned last_nonzero_ms(const int32_t *x, unsigned frames, int32_t floor_)
{
    for (unsigned i = frames; i-- > 0;) if (abs(x[2u * i]) > floor_) return i * 1000u / 44100u;
    return 0;
}
static unsigned below_ms(const int32_t *x, unsigned frames, double peak_db)   /* first time a 20 ms window stays under peak - peak_db */
{
    double peak = 0; for (unsigned t = 0; t + 20u < frames * 1000u / 44100u; t += 5u) { double r = win_rms(x, t, 20u); if (r > peak) peak = r; }
    double lim = peak * pow(10.0, -peak_db / 20.0); unsigned t_peak = 0;
    for (unsigned t = 0; t + 20u < frames * 1000u / 44100u; t += 5u) if (win_rms(x, t, 20u) >= peak * 0.9) t_peak = t;
    for (unsigned t = t_peak; t + 20u < frames * 1000u / 44100u; t += 5u) if (win_rms(x, t, 20u) < lim) return t;
    return 0;
}

static void env_numbers(void)
{
    br_app_init(); fm1_ms = 2000u; br_load_preset(BR_DRUM_PART, BR_KIT_PLAITS);
    br_sound_t s = br_part[BR_DRUM_PART].sound; br_env_t *e = &br_env[BR_DRUM_PART];
    printf("ENVELOPE (kit defaults: attack %u decay %u sustain %u release %u):\n", s.attack, s.decay, s.sustain, s.release);
    br_env_start(BR_DRUM_PART, 36u, 120u, &s, 0u); br_env_refresh(e, &s, 0u);
    for (uint32_t i = 0; i < 60000u; ++i) br_env_advance(e);
    printf("  after 1.36 s from a hit: stage %u amp %u (sustain %u): the sustain holds at full\n", e->stage, e->amp, e->sustain);
    e->stage = BR_ENV_RELEASE; e->release_start = e->amp; br_env_refresh(e, &s, 0u);
    printf("  release: %u samples = %.0f ms; amp (dB re full) after 10,25,50,100,200,300,500 ms: ", e->release, e->release * 1000.0 / 44100.0);
    uint32_t t = 0; static const unsigned ms[] = {10,25,50,100,200,300,500};
    for (unsigned k = 0; k < 7; ++k) { while (t < ms[k] * 44100u / 1000u) { br_env_advance(e); ++t; } printf("%.1f ", e->amp ? 20.0 * log10(e->amp / 32767.0) : -99.0); }
    printf("\n  stage after 500 ms: %u amp %u\n", e->stage, e->amp);
    /* a hit during release: the new fix reopens from the current amp */
    br_app_init(); fm1_ms = 2000u; br_load_preset(BR_DRUM_PART, BR_KIT_PLAITS); s = br_part[BR_DRUM_PART].sound; e = &br_env[BR_DRUM_PART];
    br_env_start(BR_DRUM_PART, 36u, 120u, &s, 0u); br_env_refresh(e, &s, 0u); for (uint32_t i = 0; i < 4000u; ++i) br_env_advance(e);
    e->stage = BR_ENV_RELEASE; e->release_start = e->amp; br_env_refresh(e, &s, 0u);
    for (unsigned d = 0; d < 3; ++d) {
        static const unsigned after_ms[] = {50, 100, 200};
        br_app_init(); fm1_ms = 2000u; br_load_preset(BR_DRUM_PART, BR_KIT_PLAITS); s = br_part[BR_DRUM_PART].sound; e = &br_env[BR_DRUM_PART];
        br_env_start(BR_DRUM_PART, 36u, 120u, &s, 0u); br_env_refresh(e, &s, 0u); for (uint32_t i = 0; i < 4000u; ++i) br_env_advance(e);
        e->stage = BR_ENV_RELEASE; e->release_start = e->amp; br_env_refresh(e, &s, 0u);
        for (uint32_t i = 0; i < after_ms[d] * 44100u / 1000u; ++i) br_env_advance(e);
        uint32_t before = e->amp; br_env_start(BR_DRUM_PART, 38u, 100u, &s, 1u); br_env_refresh(e, &s, 0u);
        uint32_t n = 0; while (e->amp < 32767u && n < 5000u) { br_env_advance(e); ++n; }
        printf("  hit %u ms into a release: amp %u (%.1f dB) -> ramps back to full over %u samples (%.2f ms)\n", after_ms[d], before, before ? 20.0 * log10(before / 32767.0) : -99.0, n, n / 44.1);
    }
}

static const char *kit_name(uint32_t k) { return k == BR_KIT_PLAITS ? "Plaits" : "Braids"; }

static void table_header(void) { printf("  %-34s %10s %9s %9s %9s %9s %9s  %s\n", "case", "", "100 ms", "250 ms", "500 ms", "1 s", "2 s", "env silent / natural -60 dB / last sample"); }
static void report(const char *name, const int32_t *with, const int32_t *held, unsigned t0_ms, unsigned frames)
{
    static const unsigned off[] = {100, 250, 500, 1000, 2000};
    printf("  %-34s %10s", name, "err dB:");
    for (unsigned k = 0; k < 5; ++k) printf(" %9.1f", db(win_rms(with, t0_ms + off[k], 50), win_rms(held, t0_ms + off[k], 50)));
    printf("  cut at %u ms / -60 dB at %u ms\n", last_nonzero_ms(with, frames, 2), below_ms(held, frames, 60.0));
}

int main(void)
{
    static const uint32_t kits[] = { BR_KIT_PLAITS, BR_KIT_BRAIDS };
    const unsigned frames = MAXB * BR_BLOCK;
    env_numbers();
    printf("\nLIVE NOTES (a key tapped): level error of the ringing voice from the part envelope (with it vs held open), 50 ms windows t after the hit\n");
    for (unsigned ki = 0; ki < 2; ++ki)
        for (unsigned dec = 0; dec < 2; ++dec) {
            uint32_t decay = dec ? 127u : 64u;
            printf("[%s kit, voice decay %u]\n", kit_name(kits[ki]), decay); table_header();
            static const struct { const char *name; unsigned off_ms; } taps[] = { {"kick, key released after 120 ms", 120}, {"kick, key released after 1 s", 1000} };
            for (unsigned k = 0; k < 2; ++k) {
                ev_t ev[2] = { {0, 1, 0}, { taps[k].off_ms * 44100u / 1000u / BR_BLOCK, 0, 0 } };
                render(kits[ki], decay, 128u, 0, 0xFFFFFFFFu, ev, 2, MAXB, 0, 0, g_out[0]);
                render(kits[ki], decay, 128u, 0, 0xFFFFFFFFu, ev, 2, MAXB, 1, 0, g_out[1]);
                report(taps[k].name, g_out[0], g_out[1], 0, frames);
            }
            static const struct { const char *name; unsigned off_ms; } hats[] = { {"kick + hat at 250 ms, hat off 50 ms later", 300}, {"kick + hat at 250 ms, hat off 400 ms later", 650} };
            for (unsigned k = 0; k < 2; ++k) {
                ev_t ev[3] = { {0, 1, 0}, { 250u * 44100u / 1000u / BR_BLOCK, 1, 2 }, { hats[k].off_ms * 44100u / 1000u / BR_BLOCK, 0, 2 } };
                render(kits[ki], decay, 128u, 0, 0xFFFFFFFFu, ev, 3, MAXB, 0, 0, g_out[0]);
                render(kits[ki], decay, 128u, 0, 0xFFFFFFFFu, ev, 3, MAXB, 1, 0, g_out[1]);
                report(hats[k].name, g_out[0], g_out[1], 0, frames);
            }
        }
    printf("\nEVERY VOICE of each lane (key tapped 120 ms, 50 ms windows 100/250/500/1000/2000 ms after the hit; default voice decay 64)\n");
    for (unsigned ki = 0; ki < 2; ++ki) {
        printf("[%s kit]\n", kit_name(kits[ki])); table_header();
        for (uint32_t lane = 0; lane < BR_DRUM_LANES; ++lane)
            for (uint32_t choice = 0; choice < 3; ++choice) {
                uint32_t mask = br_drum_set_lane_choice(br_part[BR_DRUM_PART].sound.drum_voice_mask, lane, choice);
                char name[48]; snprintf(name, sizeof name, "lane %u choice %u (voice %u)", lane, choice, br_drum_voice_id(mask, lane));
                ev_t ev[2] = { {0, 1, lane}, { 120u * 44100u / 1000u / BR_BLOCK, 0, lane } };
                render(kits[ki], 64u, 128u, 0, mask, ev, 2, MAXB, 0, 0, g_out[0]);
                render(kits[ki], 64u, 128u, 0, mask, ev, 2, MAXB, 1, 0, g_out[1]);
                report(name, g_out[0], g_out[1], 0, frames);
            }
    }
    printf("\nSEQUENCER-DRIVEN kit steps (no note-offs are sent for kit steps; the part envelope is released only by a transport stop). Level error from the envelope over 4 s, kick on step 0 only, 16 steps:\n");
    for (unsigned ki = 0; ki < 2; ++ki)
        for (unsigned bpm = 60; bpm <= 180; bpm += 60)
            for (unsigned set = 0; set < 3; ++set) {
                static const struct { uint32_t decay, gate; const char *n; } sets[] = { {64, 128, "default"}, {127, 0, "decay 127 gate 0"}, {0, 127, "decay 0 gate 127"} };
                for (int hold = 0; hold < 2; ++hold) {
                    br_app_init(); fm1_ms = 2000u; br_load_preset(kits[ki], 0u); br_load_preset(BR_DRUM_PART, kits[ki]); set_sound(sets[set].decay, sets[set].gate, 0, 0xFFFFFFFFu);
                    br_kit_env_hold_probe = (uint32_t)hold; br_bpm = (int)bpm;
                    for (uint32_t st = 0; st < 16u; ++st) { br_part[BR_DRUM_PART].step[st] = st == 0 ? 1u : 0u;
                        for (uint32_t lane = 0; lane < BR_DRUM_LANES; ++lane) { br_seq_kit_velocity[st][lane] = 120u; br_seq_kit_length[st][lane] = 0u; br_seq_kit_condition[st][lane] = BR_STEP_COND_ALWAYS;
                            br_seq_kit_note[st][lane] = (uint8_t)br_drum_voice_id(br_part[BR_DRUM_PART].sound.drum_voice_mask, lane); br_seq_kit_pitch[st][lane] = BR_STEP_PITCH_DEFAULT; br_seq_kit_micro[st][lane] = 0; br_seq_kit_retrig[st][lane] = 0; } }
                    br_playing = 1u;
                    for (unsigned b = 0; b < MAXB; ++b) { memset(&g_out[hold][b * BR_BLOCK * 2u], 0, BR_BLOCK * 2u * 4u); assert(app_audio(&g_out[hold][b * BR_BLOCK * 2u], BR_BLOCK) == FM1_AUDIO_RENDER_OK); }
                    if (!hold) { static unsigned shown; if (!shown++ && ki == 0) {} }
                }
                { unsigned bytes_differ = memcmp(g_out[0], g_out[1], MAXB * BR_BLOCK * 8u) != 0; unsigned stage = br_env[BR_DRUM_PART].stage, amp = br_env[BR_DRUM_PART].amp;
                  printf("  %-6s %3u BPM %-18s: envelope output %s the held-open output; end state stage %u amp %u\n", kit_name(kits[ki]), bpm, sets[set].n, bytes_differ ? "DIFFERS from" : "is identical to", stage, amp); }
            }
    printf("\nTRANSPORT STOP while a kick rings (stop 120 ms after the hit; with the envelope released as today vs held open), 50 ms windows:\n"); table_header();
    for (unsigned ki = 0; ki < 2; ++ki) {
        for (int hold = 0; hold < 2; ++hold) {
            br_app_init(); fm1_ms = 2000u; br_load_preset(BR_DRUM_PART, kits[ki]); set_sound(64u, 128u, 0, 0xFFFFFFFFu); br_kit_env_hold_probe = (uint32_t)hold;
            for (unsigned b = 0; b < MAXB; ++b) {
                if (b == 0) br_engine_note_on(BR_DRUM_PART, br_drum_lane_note(0u), 120u);
                if (b == 120u * 44100u / 1000u / BR_BLOCK) br_engine_all_off();
                memset(&g_out[hold][b * BR_BLOCK * 2u], 0, BR_BLOCK * 8u); assert(app_audio(&g_out[hold][b * BR_BLOCK * 2u], BR_BLOCK) == FM1_AUDIO_RENDER_OK);
            }
        }
        report(ki ? "Braids kick, stop at 120 ms" : "Plaits kick, stop at 120 ms", g_out[0], g_out[1], 0, frames);
    }
    printf("\nWHY BRAIDS SHOWS NO FIRST-HIT RAMP: lengthen the part envelope's attack (sound.attack) and compare the first kick opened by the hit with the envelope already open\n");
    for (unsigned ki = 0; ki < 2; ++ki) for (unsigned atk = 5; atk <= 100; atk += 47) {
        ev_t ev[1] = { {0, 1, 0} };
        for (int prime = 0; prime < 2; ++prime) {
            br_app_init(); fm1_ms = 2000u; br_load_preset(BR_DRUM_PART, kits[ki]);
            br_part[BR_DRUM_PART].revision++; br_part[BR_DRUM_PART].sound.attack = atk; RING_PUBLISH(); br_part[BR_DRUM_PART].revision++;
            if (prime) { br_env_t *e = &br_env[BR_DRUM_PART]; br_sound_t sd = br_part[BR_DRUM_PART].sound; br_env_start(BR_DRUM_PART, 36u, 100u, &sd, 0u); e->stage = BR_ENV_SUSTAIN; e->amp = 32767u; e->pos = 32767u << 16; }
            for (unsigned b = 0; b < 6; ++b) { if (b == 0) br_engine_note_on(BR_DRUM_PART, br_drum_lane_note(0u), 120u); memset(&g_out[prime][b * BR_BLOCK * 2u], 0, BR_BLOCK * 8u); assert(app_audio(&g_out[prime][b * BR_BLOCK * 2u], BR_BLOCK) == FM1_AUDIO_RENDER_OK); }
            (void)ev;
        }
        double pa = 0, pb = 0; unsigned diff = 0; for (unsigned i = 0; i < 6u * BR_BLOCK; ++i) { double a = g_out[0][2u * i], b = g_out[1][2u * i]; if (fabs(a) > pa) pa = fabs(a); if (fabs(b) > pb) pb = fabs(b); diff += g_out[0][2u * i] != g_out[1][2u * i]; }
        printf("  %-6s attack %3u: peak %.0f vs %.0f, %u samples differ; first samples (hit-opened): %d %d %d %d, (already open): %d %d %d %d\n", kit_name(kits[ki]), atk, pa, pb, diff,
               g_out[0][2], g_out[0][4], g_out[0][6], g_out[0][8], g_out[1][2], g_out[1][4], g_out[1][6], g_out[1][8]);
    }
    printf("\nFIRST-HIT RAMP: a first hit from OFF with the envelope's 32-sample attack (as today) against the same hit with the part envelope forced fully open by the hit (no ramp); peak loss and energy loss in the first 1 ms (44 frames), and in the first 32 frames\n");
    printf("  %-8s %-26s %12s %14s %14s\n", "kit", "voice / setting", "peak loss dB", "first 1 ms dB", "first 32 fr dB");
    for (unsigned ki = 0; ki < 2; ++ki)
        for (uint32_t lane = 0; lane < BR_DRUM_LANES; ++lane)
            for (uint32_t choice = 0; choice < 3; ++choice)
                for (unsigned set = 0; set < 3; ++set) {
                    static const struct { uint32_t decay; int tune; const char *n; } sets[] = { {64, 0, "default"}, {10, -24, "short, low"}, {127, 24, "long, high"} };
                    uint32_t mask = br_drum_set_lane_choice(br_part[BR_DRUM_PART].sound.drum_voice_mask, lane, choice);
                    ev_t ev[1] = { {0, 1, lane} };
                    render(kits[ki], sets[set].decay, 128u, sets[set].tune, mask, ev, 1, 6, 0, 0, g_out[0]);      /* today: attack 5, a 32-sample ramp */
                    render(kits[ki], sets[set].decay, 128u, sets[set].tune, mask, ev, 1, 6, 2, 0, g_out[1]);      /* the envelope forced fully open by the hit */
                    double pa = 0, pb = 0, ea = 0, eb = 0, e32a = 0, e32b = 0;
                    for (unsigned i = 0; i < 6u * BR_BLOCK; ++i) { double a = g_out[0][2u * i], b = g_out[1][2u * i]; if (fabs(a) > pa) pa = fabs(a); if (fabs(b) > pb) pb = fabs(b); if (i < 44) { ea += a * a; eb += b * b; } if (i < 32) { e32a += a * a; e32b += b * b; } }
                    if (pb < 50.0) continue;   /* silent voice */
                    printf("  %-8s lane %u voice %-2u %-12s %12.2f %14.2f %14.2f\n", kit_name(kits[ki]), lane, br_drum_voice_id(mask, lane), sets[set].n, db(pa, pb), db(sqrt(ea), sqrt(eb)), db(sqrt(e32a), sqrt(e32b)));
                }
    if (getenv("KIT_RELEASE_OUT")) {   /* listening files: today vs the part envelope held open, the worst cases */
        static const struct { const char *name; uint32_t kit; int n; ev_t ev[3]; } cases[] = {
            { "plaits_kick_key_tapped_120ms", BR_KIT_PLAITS, 2, { {0, 1, 0}, {120u * 44100u / 1000u / BR_BLOCK, 0, 0} } },
            { "plaits_kick_then_hat_off_50ms", BR_KIT_PLAITS, 3, { {0, 1, 0}, {250u * 44100u / 1000u / BR_BLOCK, 1, 2}, {300u * 44100u / 1000u / BR_BLOCK, 0, 2} } },
            { "braids_kick_then_hat_off_50ms", BR_KIT_BRAIDS, 3, { {0, 1, 0}, {250u * 44100u / 1000u / BR_BLOCK, 1, 2}, {300u * 44100u / 1000u / BR_BLOCK, 0, 2} } },
        };
        for (unsigned c = 0; c < 3; ++c)
            for (int hold = 0; hold < 2; ++hold) {
                char path[512]; render(cases[c].kit, 64u, 128u, 0, 0xFFFFFFFFu, cases[c].ev, (unsigned)cases[c].n, MAXB, hold, 0, g_out[0]);
                snprintf(path, sizeof path, "%s/%s_%s.raw", getenv("KIT_RELEASE_OUT"), cases[c].name, hold ? "envelope_held_open" : "today");
                FILE *f = fopen(path, "wb"); fwrite(g_out[0], 4, (size_t)MAXB * BR_BLOCK * 2u, f); fclose(f);
            }
    }
    return 0;
}
