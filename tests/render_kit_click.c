/* Renders a short kit pattern through the whole app to a raw file of int32 stereo frames (for listening files and difference figures):
 * kick with a long tail, hats and a snare on top. Usage: render_kit_click <plaits|braids|808|909> <out.raw> [blocks]. A tool, not a test. */
#define DINGHY_HOST 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    uint32_t preset = !strcmp(argv[1], "plaits") ? BR_KIT_PLAITS : !strcmp(argv[1], "braids") ? BR_KIT_BRAIDS : !strcmp(argv[1], "808") ? BR_KIT_808 : BR_KIT_909, blocks = argc > 3 ? (uint32_t)atoi(argv[3]) : 340u;
    FILE *f = fopen(argv[2], "wb");
    static const uint16_t steps[16] = { 1u, 0u, 4u, 0u, 4u, 0u, 4u, 8u, 2u, 0u, 4u, 0u, 4u, 0u, 4u, 8u }; /* lane 0 kick, 1 snare, 2 hat, 3 hat 2 */
    br_app_init(); fm1_ms = 2000u;
    br_load_preset(BR_DRUM_PART, preset);
    fprintf(stderr, "preset requested %u, loaded %u, mode %u\n", preset, br_part[BR_DRUM_PART].sound.preset, br_mode[BR_DRUM_PART]);
    br_bpm = 120;
    for (uint32_t s = 0; s < 16u; ++s) {
        br_part[BR_DRUM_PART].step[s] = steps[s];
        for (uint32_t lane = 0; lane < BR_DRUM_LANES; ++lane) {
            br_seq_kit_velocity[s][lane] = 120u; br_seq_kit_length[s][lane] = 0u; br_seq_kit_condition[s][lane] = BR_STEP_COND_ALWAYS;
            br_seq_kit_note[s][lane] = (uint8_t)br_drum_voice_id(br_part[BR_DRUM_PART].sound.drum_voice_mask, lane);
            br_seq_kit_pitch[s][lane] = BR_STEP_PITCH_DEFAULT; br_seq_kit_micro[s][lane] = 0; br_seq_kit_retrig[s][lane] = 0;
        }
    }
    if (argc > 4) { br_env_t *e = &br_env[BR_DRUM_PART]; br_env_start(BR_DRUM_PART, 36u, 100u, &br_part[BR_DRUM_PART].sound, 0u); e->stage = BR_ENV_SUSTAIN; e->amp = 32767u; e->pos = 32767u << 16; } /* env already open: no attack ramp on the first hit */
    br_playing = 1u;
    for (uint32_t b = 0; b < blocks; ++b) {
        int32_t audio[BR_BLOCK * 2u] = {0};
        assert(app_audio(audio, BR_BLOCK) == FM1_AUDIO_RENDER_OK);
        fwrite(audio, sizeof audio[0], BR_BLOCK * 2u, f);
    }
    fclose(f);
    return 0;
}
