/* SPDX-License-Identifier: GPL-3.0-only
 * BR_FX_TAIL_EARLY_END: the same scenarios render with the switch off and on (two forked children of one process image,
 * the switch a runtime word); every output sample must be identical up to the NEXT note after a pause, and the report
 * says how many track-FX blocks ran (tail_samples != 0 after the block) with each setting. Not a state comparison:
 * the point of the switch is that the state differs (the tail ends earlier). Output differences after the next note are
 * reported, not asserted (they come only from the free-running LFO/wow phase of a tail that ended earlier). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
static unsigned br_tail_early_word;
#define BR_FX_TAIL_EARLY_END (br_tail_early_word & 1u)
#define DINGHY_HOST 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main

#define MAXBLK 3600
static int32_t *shared_out;          /* [mode][scenario][block][512] */
static unsigned *shared_stat;        /* [mode][scenario][2]: blocks with a tail running, last nonzero block */
static int32_t ob[BR_BLOCK * 2u];
enum { SCEN = 8 };

static unsigned gr_blocks;
static void render_blocks(int mode, int sc, int *block, int n, unsigned *tail_blocks, unsigned *last_nz)
{
    for (int b = 0; b < n; ++b, ++*block) {
        uint32_t r = platform_audio_render_half(ob, BR_BLOCK);
        if (r != FM1_AUDIO_RENDER_OK) { printf("render failed\n"); exit(1); }
        memcpy(&shared_out[(((size_t)mode * SCEN + sc) * MAXBLK + *block) * 512u], ob, sizeof ob);
        for (uint32_t p = 0; p < 4u; ++p) if (br_fx_track[p].tail_samples) { ++*tail_blocks; break; }
        gr_blocks += br_gr_run ? 1u : 0u;
        for (uint32_t i = 0; i < 512u; ++i) if (ob[i]) { *last_nz = (unsigned)*block; break; }
    }
}

static void scenario(int mode, int sc)
{
    br_app_init(); fm1_ms = 1000u;
    br_master.revision++; RING_PUBLISH();
    int block = 0; unsigned tail_blocks = 0, last_nz = 0;
    /* delay engine, time, feedback, tempo, mod engine/mix, reverb send, drive per scenario */
    static const struct { uint32_t eng, time, fb, bpm, modeng, modmix, delay, reverb, drive; } cfg[SCEN] = {
        {0, 77, 44, 120, 0, 0, 60, 0, 0},      /* plain digital delay */
        {0, 127, 110, 40, 0, 0, 70, 0, 0},     /* longest time, slow tempo, high feedback: echoes return after long silences */
        {1, 100, 40, 90, 0, 0, 50, 30, 0},     /* tape, moderate feedback, reverb send */
        {2, 90, 40, 100, 0, 64, 60, 40, 0},    /* multitap + chorus */
        {3, 100, 40, 100, 2, 80, 60, 20, 30},  /* glitch + phaser + drive */
        {0, 60, 0, 120, 1, 90, 0, 0, 0},       /* modulation only (flanger) */
        {0, 127, 60, 60, 3, 40, 50, 0, 0},     /* long time + vibrato */
        {0, 77, 60, 120, 0, 0, 50, 0, 0},      /* second note after a pause */
    };    for (uint32_t p = 0; p < 3u; ++p) {
        br_part[p].fx.delay_engine = cfg[sc].eng; br_part[p].fx.delay_time = cfg[sc].time;
        br_part[p].fx.delay_feedback = cfg[sc].fb; br_part[p].fx.mod_engine = cfg[sc].modeng;
        br_part[p].fx.mod_mix = cfg[sc].modmix; br_part[p].fx.mod_depth = 64u; br_part[p].fx.mod_rate = 50u;
        br_part[p].sound.delay = cfg[sc].delay; br_part[p].sound.reverb = cfg[sc].reverb; br_part[p].sound.drive = cfg[sc].drive;
        br_part[p].revision += 2u;
    }
    br_bpm = cfg[sc].bpm;
    render_blocks(mode, sc, &block, 20, &tail_blocks, &last_nz);
    for (int k = 0; k < 8; ++k) {                      /* 8 notes over ~3 s */
        for (uint32_t p = 0; p < 3u; ++p) br_engine_note_on(p, 48u + p * 7u + (uint32_t)k, 110u);
        render_blocks(mode, sc, &block, 30, &tail_blocks, &last_nz);
        for (uint32_t p = 0; p < 3u; ++p) br_engine_note_off(p, 48u + p * 7u + (uint32_t)k);
        render_blocks(mode, sc, &block, 10, &tail_blocks, &last_nz);
    }
    render_blocks(mode, sc, &block, 2600, &tail_blocks, &last_nz);        /* the tail: about 15 s */
    int quiet_end = block;
    if (sc == SCEN - 1) {                               /* a new note after the pause */
        for (uint32_t p = 0; p < 3u; ++p) br_engine_note_on(p, 55u + p, 100u);
        render_blocks(mode, sc, &block, 150, &tail_blocks, &last_nz);
    }
    shared_stat[((size_t)mode * SCEN + sc) * 5u + 0u] = tail_blocks;
    shared_stat[((size_t)mode * SCEN + sc) * 5u + 1u] = last_nz;
    shared_stat[((size_t)mode * SCEN + sc) * 5u + 2u] = (unsigned)block;
    shared_stat[((size_t)mode * SCEN + sc) * 5u + 3u] = (unsigned)quiet_end;
    shared_stat[((size_t)mode * SCEN + sc) * 5u + 4u] = gr_blocks; gr_blocks = 0u;
}

int main(void)
{
    size_t bytes = (size_t)2 * SCEN * MAXBLK * 512u * sizeof(int32_t);
    shared_out = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    shared_stat = mmap(0, 2u * SCEN * 5u * sizeof(unsigned), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (shared_out == MAP_FAILED || shared_stat == MAP_FAILED) return 1;
    for (int mode = 0; mode < 2; ++mode) {
        pid_t pid = fork();
        if (!pid) {
            br_tail_early_word = mode ? ~0u : 0u;
            for (int sc = 0; sc < SCEN; ++sc) scenario(mode, sc);
            _exit(0);
        }
        int st; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("child failed\n"); return 1; }
    }
    int bad = 0;
    for (int sc = 0; sc < SCEN; ++sc) {
        unsigned *a = &shared_stat[((size_t)0 * SCEN + sc) * 5u], *b = &shared_stat[((size_t)1 * SCEN + sc) * 5u];
        unsigned blocks = a[2], quiet_end = a[3];
        int32_t maxdiff_before = 0, maxdiff_after = 0; unsigned first_diff = 0, ndiff = 0;
        for (unsigned blk = 0; blk < blocks; ++blk) {
            const int32_t *x = &shared_out[(((size_t)0 * SCEN + sc) * MAXBLK + blk) * 512u];
            const int32_t *y = &shared_out[(((size_t)1 * SCEN + sc) * MAXBLK + blk) * 512u];
            for (unsigned i = 0; i < 512u; ++i) {
                int32_t d = x[i] > y[i] ? x[i] - y[i] : y[i] - x[i];
                if (!d) continue;
                if (!ndiff) first_diff = blk;
                ++ndiff;
                if (blk < quiet_end) { if (d > maxdiff_before) maxdiff_before = d; }
                else if (d > maxdiff_after) maxdiff_after = d;
            }
        }
        printf("scenario %d: output max diff until the pause ends %d (>>7: %d LSB16), after the next note %d (%d LSB16), differing samples %u (first block %u of %u); "
               "FX-tail blocks off %u on %u (saved %u); last non-zero output block off %u on %u; shared reverb running blocks off %u on %u\n",
               sc, maxdiff_before, maxdiff_before >> 7, maxdiff_after, maxdiff_after >> 7, ndiff, first_diff, blocks, a[0], b[0], a[0] - b[0], a[1], b[1], a[4], b[4]);
        if (maxdiff_before) bad = 1;
        if (a[1] != b[1]) bad = 1;         /* the last audible sample must not move */
    }
    if (bad) { printf("FAIL: output lost before the next note\n"); return 1; }
    printf("tail early end: output identical to the end of the pause in every scenario\n");
    return 0;
}
