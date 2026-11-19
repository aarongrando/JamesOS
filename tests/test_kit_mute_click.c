/* A muted kit lane must be silent when it is hit, and must not disturb the other lanes. Through the real app (engine, post, track FX,
 * master): for each kit preset, each muted lane and each combination of the other lanes muted, with the compressor off and on:
 *  (a) a pattern that hits only the muted lane renders exactly zero;
 *  (b) a pattern where a still-audible lane plays and the muted lane is also hit (between the audible hits and on the same step)
 *      renders sample for sample the same as the pattern without the muted lane's hits.
 * Sequencer steps and live note-ons are both used. Prints the worst difference and where it is. */
#define DINGHY_HOST 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define BLOCKS 140u
static int32_t g_a[BLOCKS * BR_BLOCK * 2u], g_b[BLOCKS * BR_BLOCK * 2u];
/* The drum track allows two kits (br_preset_allowed_on_part): Plaits (mode 4) and Braids (mode 1, the default). 808 and 909 are retired. */
static const uint32_t presets[] = { BR_KIT_PLAITS, BR_KIT_BRAIDS };

static void publish_masks(uint32_t mute)
{
    br_part[BR_DRUM_PART].revision++;
    br_kit_mute_mask = mute; br_kit_solo_mask = 0u;
    RING_PUBLISH(); br_part[BR_DRUM_PART].revision++;
}
/* steps[s] = lane mask hit on step s (16 steps). live != 0: the hits are note-ons at the block starting each step instead. */
static void render(uint32_t preset, uint32_t mute, const uint16_t steps[16], int live, int comp, int32_t *out)
{
    br_app_init(); fm1_ms = 2000u;
    br_load_preset(BR_DRUM_PART, preset);
    br_bpm = 240;
    if (comp) {
        br_master.revision++;
        br_master.comp.source = BR_COMP_ALL; br_master.comp.threshold = 40u; br_master.comp.ratio = 90u;
        br_master.comp.makeup = 6u; br_master.comp.mix = 127u;
        RING_PUBLISH(); br_master.revision++;
    }
    publish_masks(mute);
    if (!live) {
        for (uint32_t s = 0; s < 16u; ++s) {
            br_part[BR_DRUM_PART].step[s] = steps[s];
            for (uint32_t lane = 0; lane < BR_DRUM_LANES; ++lane) {
                br_seq_kit_velocity[s][lane] = 120u; br_seq_kit_length[s][lane] = 0u;
                br_seq_kit_condition[s][lane] = BR_STEP_COND_ALWAYS;
                br_seq_kit_note[s][lane] = (uint8_t)br_drum_voice_id(br_part[BR_DRUM_PART].sound.drum_voice_mask, lane);
                br_seq_kit_pitch[s][lane] = BR_STEP_PITCH_DEFAULT; br_seq_kit_micro[s][lane] = 0; br_seq_kit_retrig[s][lane] = 0;
            }
        }
        br_playing = 1u;
    }
    for (uint32_t b = 0; b < BLOCKS; ++b) {
        if (live && b % 12u == 0u) {
            uint32_t mask = steps[(b / 12u) & 15u];
            for (uint32_t lane = 0; lane < BR_DRUM_LANES; ++lane)
                if (mask & (1u << lane)) br_engine_note_on(BR_DRUM_PART, br_drum_lane_note(lane), 120u);
        }
        if (live && b % 12u == 5u) {   /* the key is released again, as a MIDI controller would */
            uint32_t mask = steps[(b / 12u) & 15u];
            for (uint32_t lane = 0; lane < BR_DRUM_LANES; ++lane)
                if (mask & (1u << lane)) br_engine_note_off(BR_DRUM_PART, br_drum_lane_note(lane));
        }
        memset(&out[b * BR_BLOCK * 2u], 0, BR_BLOCK * 2u * sizeof(int32_t));
        assert(app_audio(&out[b * BR_BLOCK * 2u], BR_BLOCK) == FM1_AUDIO_RENDER_OK);
    }
}

static unsigned g_runs, g_fail;
static int32_t g_worst; static char g_where[160];

static void compare(const char *what, uint32_t preset, uint32_t mute, int live, int comp, const int32_t *x, const int32_t *y, int must_be_zero)
{
    int32_t worst = 0; uint32_t at = 0;
    for (uint32_t i = 0; i < BLOCKS * BR_BLOCK * 2u; ++i) {
        int32_t d = must_be_zero ? x[i] : x[i] - y[i];
        if (d < 0) d = -d;
        if (d > worst) { worst = d; at = i; }
    }
    ++g_runs;
    if (worst) {
        ++g_fail;
        if (worst > g_worst) { g_worst = worst; snprintf(g_where, sizeof g_where, "%s preset %u mute %x %s comp %d: sample %u (block %u offset %u ch %u)", what, preset, mute, live ? "live" : "seq", comp, at, at / (BR_BLOCK * 2u), (at % (BR_BLOCK * 2u)) / 2u, at & 1u); }
        if (g_fail <= 3u && !must_be_zero) {   /* the shape of the difference */
            uint32_t n = 0, first = 0, last = 0;
            for (uint32_t i = 0; i < BLOCKS * BR_BLOCK * 2u; ++i) if (x[i] != y[i]) { if (!n) first = i; last = i; ++n; }
            fprintf(stderr, "  %u samples differ, from %u to %u; around the first:", n, first, last);
            for (uint32_t i = first; i < first + 24u && i < BLOCKS * BR_BLOCK * 2u; i += 2u) fprintf(stderr, " %d", x[i] - y[i]);
            fprintf(stderr, "\n");
        }
        if (1) fprintf(stderr, "DIFF %s preset %u mute %x %s comp %d: max %d at sample %u (block %u offset %u ch %u)\n", what, preset, mute, live ? "live" : "seq", comp, worst, at, at / (BR_BLOCK * 2u), (at % (BR_BLOCK * 2u)) / 2u, at & 1u);
    }
}

/* The audible case, at the part envelope that every lane's output is multiplied by: a hit on one lane while the envelope is open
 * (a kick still ringing) must not lower it, restart its attack or change its stage. From OFF the first hit starts it as before, and
 * from release a hit opens it again from where it is. With BR_KIT_ENV_NO_RESTART=0 this fails (the amp falls to the first attack
 * step, about -30 dB, and ramps back over 32 samples: a hole in everything that is sounding). Returns the number of failures. */
static unsigned check_env_hits(void)
{
    unsigned bad = 0;
    for (uint32_t pi = 0; pi < sizeof presets / sizeof presets[0]; ++pi) {
        br_app_init(); fm1_ms = 2000u;
        br_load_preset(BR_DRUM_PART, presets[pi]);
        br_sound_t snd = br_part[BR_DRUM_PART].sound; br_env_t *e = &br_env[BR_DRUM_PART];
        br_kit_env_hit(BR_DRUM_PART, 36u, 120u, &snd);                      /* first hit from OFF: as before */
        if (e->stage != BR_ENV_ATTACK || e->amp != 0u) { ++bad; fprintf(stderr, "ENV first hit: stage %u amp %u\n", e->stage, e->amp); }
        br_env_refresh(e, &snd, 0u);
        static const uint32_t after[] = { 0u, 5u, 31u, 33u, 500u, 4410u, 30000u };   /* samples after the first hit: attack, decay, sustain */
        for (uint32_t k = 0; k < sizeof after / sizeof after[0]; ++k) {
            br_app_init(); fm1_ms = 2000u; br_load_preset(BR_DRUM_PART, presets[pi]); snd = br_part[BR_DRUM_PART].sound; e = &br_env[BR_DRUM_PART];
            br_kit_env_hit(BR_DRUM_PART, 36u, 120u, &snd); br_env_refresh(e, &snd, 0u);
            for (uint32_t i = 0; i < after[k]; ++i) br_env_advance(e);
            uint32_t amp = e->amp, stage = e->stage, pos = e->pos;
            br_kit_env_hit(BR_DRUM_PART, 42u, 100u, &snd); br_env_refresh(e, &snd, 0u);
            if (e->amp != amp || e->stage != stage || e->pos != pos || e->note != 42u || e->velocity != 100u) {
                ++bad; fprintf(stderr, "ENV hit %u samples after the first: amp %u -> %u, stage %u -> %u (a hole)\n", after[k], amp, e->amp, stage, e->stage);
            }
            for (uint32_t i = 0; i < 40u; ++i) { br_env_advance(e); if (e->amp < amp && stage != BR_ENV_ATTACK) { ++bad; fprintf(stderr, "ENV amp fell after the hit\n"); break; } }
        }
        /* from release: a hit opens it again from where it is, not from 0 */
        br_app_init(); fm1_ms = 2000u; br_load_preset(BR_DRUM_PART, presets[pi]); snd = br_part[BR_DRUM_PART].sound; e = &br_env[BR_DRUM_PART];
        br_kit_env_hit(BR_DRUM_PART, 36u, 120u, &snd); br_env_refresh(e, &snd, 0u);
        for (uint32_t i = 0; i < 2000u; ++i) br_env_advance(e);
        br_release_note(BR_DRUM_PART, 36u); br_env_refresh(e, &snd, 0u);
        for (uint32_t i = 0; i < 300u; ++i) br_env_advance(e);
        { uint32_t amp = e->amp;
          br_kit_env_hit(BR_DRUM_PART, 38u, 110u, &snd); br_env_refresh(e, &snd, 0u);
          if (amp == 0u || e->stage == BR_ENV_OFF || (BR_KIT_ENV_NO_RESTART && e->amp != amp)) { ++bad; fprintf(stderr, "ENV hit in release: amp %u -> %u stage %u\n", amp, e->amp, e->stage); } }
    }
    return bad;
}

int main(int argc, char **argv)
{
    unsigned env_bad = check_env_hits();
    printf("kit envelope: %u failures\n", env_bad);
    if (env_bad) return 1;
    uint32_t quick = argc > 1 && argv[1][0] == 'q';
    for (uint32_t pi = 0; pi < sizeof presets / sizeof presets[0]; ++pi)
        for (uint32_t m = 0; m < BR_DRUM_LANES; ++m)
            for (uint32_t others = 0; others < 8u; ++others) {
                uint32_t mute = 1u << m, k = 0u, audible = 0xFFFFFFFFu;
                for (uint32_t lane = 0; lane < BR_DRUM_LANES; ++lane) if (lane != m) { if (others & (1u << k)) mute |= 1u << lane; ++k; }
                for (uint32_t lane = 0; lane < BR_DRUM_LANES; ++lane) if (!(mute & (1u << lane))) { audible = lane; break; }
                if (quick && others != 0u && others != 7u) continue;
                for (int live = 0; live < 2; ++live)
                    for (int comp = 0; comp < 2; ++comp) {
                        uint16_t only[16] = {0}, with[16] = {0}, without[16] = {0};
                        for (uint32_t s = 0; s < 16u; ++s) {
                            only[s] = (s % 2u == 0u) ? (uint16_t)(1u << m) : 0u;
                            if (audible != 0xFFFFFFFFu && s % 4u == 0u) { with[s] |= (uint16_t)(1u << audible); without[s] |= (uint16_t)(1u << audible); }
                            if (s % 2u == 0u) with[s] |= (uint16_t)(1u << m);   /* the muted lane: between and on the audible hits */
                        }
                        render(presets[pi], mute, only, live, comp, g_a);
                        compare("muted lane only", presets[pi], mute, live, comp, g_a, g_a, 1);
                        if (audible != 0xFFFFFFFFu) {
                            render(presets[pi], mute, with, live, comp, g_a);
                            render(presets[pi], mute, without, live, comp, g_b);
                            compare("audible lane + muted hits", presets[pi], mute, live, comp, g_a, g_b, 0);
                            render(presets[pi], mute, without, live, comp, g_a);
                            compare("DETERMINISM of the same render", presets[pi], mute, live, comp, g_a, g_b, 0);
                        }
                    }
            }
    printf("kit mute click: %u renders compared, %u differ%s%s\n", g_runs, g_fail, g_fail ? "; worst " : "", g_fail ? g_where : "");
    if (g_fail) printf("worst difference %d\n", g_worst);
    return g_fail != 0;
}
