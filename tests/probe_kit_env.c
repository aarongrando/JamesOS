/* Probe: what the part amplitude envelope does on a kit hit today, per kit preset. Prints the envelope parameters, the amp curve after a
 * first hit from OFF, and the dip when a second hit restarts it 100, 250 and 500 ms later. A tool, not a test. */
#define DINGHY_HOST 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include <stdio.h>
#include <math.h>

static void run(const char *name, uint32_t preset)
{
    static const uint32_t delays_ms[] = {100, 250, 500};
    br_app_init(); fm1_ms = 2000u;
    br_load_preset(BR_DRUM_PART, preset);
    br_sound_t s = br_part[BR_DRUM_PART].sound; br_env_t *e = &br_env[BR_DRUM_PART];
    printf("== %s: attack %u decay %u sustain %u release %u amp_mode %u gate %u level %u\n", name, s.attack, s.decay, s.sustain, s.release, s.amp_mode, s.gate, s.level);
    br_env_start(BR_DRUM_PART, 36u, 120u, &s, 0u);
    br_env_refresh(e, &s, 0u);
    printf("   first hit from OFF: attack %u samples, decay %u, sustain %u (Q15 of 32767)\n   amp at sample 0,1,2,4,8,16,32,64,128: ", e->attack, e->decay, e->sustain);
    { uint32_t at[] = {0,1,2,4,8,16,32,64,128}, k = 0, reach = 0;
      for (uint32_t i = 0; i < 20000u; ++i) {
          br_env_advance(e);
          if (k < 9u && i == at[k]) { printf("%u ", e->amp); ++k; }
          if (!reach && e->amp >= 32767u) reach = i + 1u;
      }
      printf("\n   reaches full scale after %u samples; after 20000 samples: stage %u amp %u\n", reach, e->stage, e->amp); }
    for (unsigned d = 0; d < 3; ++d) {
        br_app_init(); fm1_ms = 2000u; br_load_preset(BR_DRUM_PART, preset);
        s = br_part[BR_DRUM_PART].sound; e = &br_env[BR_DRUM_PART];
        br_env_start(BR_DRUM_PART, 36u, 120u, &s, 0u); br_env_refresh(e, &s, 0u);
        for (uint32_t i = 0; i < delays_ms[d] * 44100u / 1000u; ++i) br_env_advance(e);
        uint32_t before = e->amp, stage = e->stage;
        br_env_start(BR_DRUM_PART, 42u, 100u, &s, 0u); br_env_refresh(e, &s, 0u);
        uint32_t minamp = before, i, back = 0, first = 0;
        for (i = 0; i < 4000u; ++i) { br_env_advance(e); if (i == 0) first = e->amp; if (e->amp < minamp) minamp = e->amp; if (!back && i > 0 && e->amp >= before) back = i; }
        printf("   second hit after %3u ms: amp %5u (stage %u) -> first sample after restart %5u (%+.1f dB), minimum %u, back to the old level after %u samples\n",
               delays_ms[d], before, stage, first, before ? 20.0 * log10((first + 0.001) / before) : 0.0, minamp, back);
    }
}
int main(void)
{
    run("PLAITS kit", BR_KIT_PLAITS); run("BRAIDS kit", BR_KIT_BRAIDS); /* 808 and 909 are not allowed on the drum track */
    return 0;
}
