/* A kit tail must survive a key release: the part envelope multiplies every lane, and a note-off used to release it (722 ms fade), cutting a
 * kick tapped for 120 ms to half its tail. With BR_KIT_ENV_NO_NOTE_OFF_RELEASE the output of a tapped kick, and of a kick followed by a tapped hat,
 * is bit-identical to the render with the part envelope held open (the probe switch exists only in this test build). The part still reaches
 * OFF and quiet afterwards (idle skips engage), a stop still ends the sound, and the stop render does not depend on the switch (its hash is
 * printed for comparison across builds). Both kits, voice decay 64 and 127. */
#define DINGHY_HOST 1
#define BR_KIT_ENV_HOLD_PROBE 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define BLOCKS 640u
static int32_t g_a[BLOCKS * BR_BLOCK * 2u], g_b[BLOCKS * BR_BLOCK * 2u];
typedef struct { uint32_t block; int on; uint32_t lane; } ev_t;
static unsigned g_quiet_block, g_env_off;

static void render(uint32_t kit, uint32_t decay, const ev_t *ev, unsigned nev, int hold, int stop_block, int32_t *out)
{
    br_app_init(); fm1_ms = 2000u; br_load_preset(BR_DRUM_PART, kit);
    br_part[BR_DRUM_PART].revision++; br_part[BR_DRUM_PART].sound.decay = decay; RING_PUBLISH(); br_part[BR_DRUM_PART].revision++;
    br_kit_env_hold_probe = (uint32_t)hold; g_quiet_block = 0; g_env_off = 0;
    for (unsigned b = 0; b < BLOCKS; ++b) {
        for (unsigned k = 0; k < nev; ++k) if (ev[k].block == b) {
            if (ev[k].on) br_engine_note_on(BR_DRUM_PART, br_drum_lane_note(ev[k].lane), 120u); else br_engine_note_off(BR_DRUM_PART, br_drum_lane_note(ev[k].lane));
        }
        if (stop_block >= 0 && b == (unsigned)stop_block) br_engine_all_off();
        memset(&out[b * BR_BLOCK * 2u], 0, BR_BLOCK * 8u);
        assert(app_audio(&out[b * BR_BLOCK * 2u], BR_BLOCK) == FM1_AUDIO_RENDER_OK);
        if (!g_quiet_block && b > 8u && br_quiet_part[BR_DRUM_PART] && br_env[BR_DRUM_PART].stage == BR_ENV_OFF) { g_quiet_block = b + 1u; g_env_off = 1; }
    }
}
static uint64_t hash(const int32_t *x, unsigned n) { uint64_t h = 1469598103934665603ull; for (unsigned i = 0; i < n; ++i) { h ^= (uint32_t)x[i]; h *= 1099511628211ull; } return h; }
static unsigned diff_samples(const int32_t *x, const int32_t *y, int32_t *worst, unsigned *first)
{
    unsigned n = 0; *worst = 0; *first = 0;
    for (unsigned i = 0; i < BLOCKS * BR_BLOCK * 2u; ++i) if (x[i] != y[i]) { int32_t d = abs(x[i] - y[i]); if (!n) *first = i / 2u; ++n; if (d > *worst) *worst = d; }
    return n;
}

int main(void)
{
    static const uint32_t kits[] = { BR_KIT_PLAITS, BR_KIT_BRAIDS };
    unsigned fails = 0;
    for (unsigned ki = 0; ki < 2; ++ki)
        for (unsigned dec = 0; dec < 2; ++dec) {
            uint32_t decay = dec ? 127u : 64u; const char *kn = ki ? "Braids" : "Plaits"; int32_t worst; unsigned first, n;
            ev_t tap[2] = { {0, 1, 0}, { 120u * 44100u / 1000u / BR_BLOCK, 0, 0} };
            render(kits[ki], decay, tap, 2, 0, -1, g_a); unsigned q_tap = g_quiet_block, off_tap = g_env_off;
            render(kits[ki], decay, tap, 2, 1, -1, g_b);
            n = diff_samples(g_a, g_b, &worst, &first); if (n) ++fails;
            printf("%s decay %3u kick tapped 120 ms: %s the envelope-held-open render (%u samples differ, max %d, first at frame %u); part OFF and quiet at block %u (%.0f ms) [%s]\n", kn, decay, n ? "DIFFERS from" : "equals", n, worst, first, q_tap, q_tap * 256.0 / 44.1, off_tap ? "reached" : "NOT reached");
            if (!off_tap) ++fails;
            ev_t hat[4] = { {0, 1, 0}, { 250u * 44100u / 1000u / BR_BLOCK, 1, 2}, { 300u * 44100u / 1000u / BR_BLOCK, 0, 2}, { 3u, 0, 0} };
            render(kits[ki], decay, hat, 3, 0, -1, g_a); unsigned q_hat = g_quiet_block, off_hat = g_env_off;
            render(kits[ki], decay, hat, 3, 1, -1, g_b);
            n = diff_samples(g_a, g_b, &worst, &first); if (n) ++fails;
            printf("%s decay %3u kick, hat tapped at 250 ms: %s the held-open render (%u differ, max %d); part OFF and quiet at block %u (%.0f ms) [%s]\n", kn, decay, n ? "DIFFERS from" : "equals", n, worst, q_hat, q_hat * 256.0 / 44.1, off_hat ? "reached" : "NOT reached");
            if (!off_hat) ++fails;
            /* stop during the ringing kick: still ends the sound; independent of the switch (compare the hash across builds) */
            render(kits[ki], decay, tap, 1, 0, 120u * 44100u / 1000u / BR_BLOCK, g_a);
            { unsigned last = 0; for (unsigned i = BLOCKS * BR_BLOCK; i-- > 0;) if (g_a[2u * i]) { last = i; break; }
              int32_t step = 0; for (unsigned i = last > 441u ? last - 441u : 1u; i <= last; ++i) { int32_t d = abs(g_a[2u * i] - g_a[2u * (i - 1)]); if (d > step) step = d; }
              printf("%s decay %3u stop at 120 ms: last non-zero sample at frame %u (%.0f ms), largest step in the last 10 ms %d; hash %016llx\n", kn, decay, last, last / 44.1, step, (unsigned long long)hash(g_a, BLOCKS * BR_BLOCK * 2u));
              if (last > 44100u) ++fails; }
        }
    printf("kit tail: %u failures\n", fails);
    return fails != 0;
}
