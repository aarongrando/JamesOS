/* SPDX-License-Identifier: GPL-3.0-only
 * Idle skips: bit-exact output and identical state against the unchanged code path
 * (tests/run_idle_skips.py builds this file twice, with every BR_IDLE_SKIP_* switch on and off, runs both
 * and compares the printed checksums). The program drives the real engine: boot, idle, notes on all parts,
 * release, decay to idle, notes again, with sends on and off, track filters low-pass / neutral / high-pass,
 * kit hits. After every block it folds the output buffer into a running hash; every 8 blocks it also folds
 * the whole DSP state (br_dsp, br_env, br_filter, br_fx_track, br_fx_track_stereo, br_comp_key, master
 * state, shared reverb) so a skip that changes any state word shows up. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
/* Host test only: the switches are runtime words, so one process image runs the scenario twice (fork) with
 * identical addresses; structures that hold pointers then compare equal. Bit 0: BR_IDLE_SKIP_KIT_LANES; bit 1: BR_IDLE_SKIP_SILENT_ROWS; bit 2: BR_IDLE_SKIP_COMP_KEY_CLEAR; bit 3: BR_IDLE_SKIP_FILTER_BYPASS; bit 4: BR_IDLE_SKIP_KIT_CONTROLS; bit 5: BR_IDLE_SKIP_BUILD_PASS2. */
static unsigned br_idle_skip_mask;
#define BR_IDLE_SKIP_KIT_LANES (br_idle_skip_mask & 1u)
#define BR_IDLE_SKIP_SILENT_ROWS ((br_idle_skip_mask >> 1) & 1u)
#define BR_IDLE_SKIP_COMP_KEY_CLEAR ((br_idle_skip_mask >> 2) & 1u)
#define BR_IDLE_SKIP_FILTER_BYPASS ((br_idle_skip_mask >> 3) & 1u)
#define BR_IDLE_SKIP_KIT_CONTROLS ((br_idle_skip_mask >> 4) & 1u)
#define BR_IDLE_SKIP_BUILD_PASS2 ((br_idle_skip_mask >> 5) & 1u)
#define DINGHY_HOST 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include <stdio.h>

static uint64_t h_comp[9];
static uint64_t h_out = 1469598103934665603ull, h_state = 1469598103934665603ull;
static void fold(uint64_t *h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; ++i) { *h ^= b[i]; *h *= 1099511628211ull; }
}
static int32_t ob[BR_BLOCK * 2u];
static void fold_part(int i, const void *p, size_t n)
{
    if (!h_comp[i]) h_comp[i] = 1469598103934665603ull;
    fold(&h_comp[i], p, n);
    fold(&h_state, p, n);
}
static void fold_state(void)
{
    fold_part(0, br_dsp, sizeof br_dsp);
    fold_part(1, br_env, sizeof br_env);
    fold_part(2, br_filter, sizeof br_filter);
    fold_part(3, br_fx_track, sizeof br_fx_track);
    {   /* the rows of a silent-skipped track are not part of the state: nobody may read them this block */
        static const int32_t zero_rows[BR_BLOCK][2];
        for (uint32_t p = 0; p < BR_PARTS; ++p)
            fold_part(4, br_fx_track_silent_seq[p] == br_fx_block_seq ? (const void *)zero_rows : (const void *)br_fx_track_stereo[p], sizeof zero_rows);
    }
    fold_part(5, br_comp_key, sizeof br_comp_key);
    fold_part(6, &br_master_state, sizeof br_master_state);
    fold_part(7, &br_gr_rv, sizeof br_gr_rv);
    fold_part(8, br_quiet_part, sizeof br_quiet_part);
}
static int blocks_run; static unsigned silent_blocks, stale_blocks;
static void run(int blocks)
{
    for (int b = 0; b < blocks; ++b) {
        uint32_t r = platform_audio_render_half(ob, BR_BLOCK);
        if (r != FM1_AUDIO_RENDER_OK) { printf("render failed\n"); exit(1); }
        fold(&h_out, ob, sizeof ob);
        for (uint32_t p = 0; p < 4u; ++p) if (br_fx_track_silent_seq[p] == br_fx_block_seq) {
            ++silent_blocks; for (uint32_t i = 0; i < BR_BLOCK; ++i) if (br_fx_track_stereo[p][i][0] | br_fx_track_stereo[p][i][1]) { ++stale_blocks; break; }
        }
        if ((++blocks_run & 7) == 0) fold_state();
    }
}
static void set_part(uint32_t p, uint32_t filter, uint32_t res, uint32_t delay, uint32_t reverb, uint32_t drive)
{
    br_part[p].sound.filter = filter; br_part[p].sound.filter_resonance = res;
    br_part[p].sound.delay = delay; br_part[p].sound.reverb = reverb; br_part[p].sound.drive = drive;
    br_part[p].revision += 2u;
}
static void phase(const char *name)
{
    fold_state();
    printf("%-34s out %016llx state %016llx\n", name, (unsigned long long)h_out, (unsigned long long)h_state);
}
static void notes_on(uint32_t base, uint32_t vel)
{
    for (uint32_t p = 0; p < 3u; ++p) br_engine_note_on(p, base + 7u * p, vel);
    br_engine_note_on(3u, 36u, vel); br_engine_note_on(3u, 38u, vel);
}
static void notes_off(uint32_t base)
{
    for (uint32_t p = 0; p < 3u; ++p) br_engine_note_off(p, base + 7u * p);
    br_engine_note_off(3u, 36u); br_engine_note_off(3u, 38u);
}

static int scenario(void)
{
    br_app_init(); fm1_ms = 1000u;
    br_master.revision++; RING_PUBLISH();
    run(300); phase("boot idle");
    /* filter x send matrix: low-pass, neutral, high-pass; sends off / on */
    static const uint32_t filters[3] = { 40u, 64u, 100u };
    for (int pass = 0; pass < 2; ++pass)
        for (int f = 0; f < 3; ++f) {
            for (uint32_t p = 0; p < 4u; ++p)
                set_part(p, filters[f], f == 1 ? 0u : 30u + 40u * (uint32_t)f, pass ? 40u : 0u, pass ? 40u : 0u, pass ? 20u : 0u);
            run(40);
            notes_on(48u + 3u * (uint32_t)f, 100u); run(60);
            br_engine_note_on(3u, 42u, 90u); br_engine_note_on(3u, 46u, 90u); run(20);
            notes_off(48u + 3u * (uint32_t)f); run(120);
            run(700);                                /* decay to idle, long enough for the 2.2 s Plaits window */
            char name[64]; snprintf(name, sizeof name, "pass %d filter %u (decayed)", pass, filters[f]);
            phase(name);
            notes_on(55u, 120u); run(30); notes_off(55u); run(200);
            snprintf(name, sizeof name, "pass %d filter %u (replay)", pass, filters[f]);
            phase(name);
        }
    /* silent track rows: meter on, pan changes (ramp on a silent track), compressor on every source, only some
     * parts sounding so silent and sounding tracks mix in one block */
    /* modulation active on every track (two LFOs, a route-free envelope target) so br_build_effective does real work */
    for (uint32_t p = 0; p < 4u; ++p) {
        br_part[p].mod.lfo[0].wave = BR_WAVE_SINE; br_part[p].mod.lfo[0].rate = 70u + 10u * p;
        br_part[p].mod.lfo[0].depth = 90u; br_part[p].mod.lfo[0].target = BR_TARGET_TIMBRE;
        br_part[p].mod.lfo[1].wave = BR_WAVE_SINE; br_part[p].mod.lfo[1].rate = 40u + 5u * p;
        br_part[p].mod.lfo[1].depth = 60u; br_part[p].mod.lfo[1].target = BR_TARGET_FILTER;
        br_part[p].mod.target = BR_TARGET_COLOR; br_part[p].mod.amount = 100;      /* the modulation envelope moves at a note-on */
        br_part[p].mod.attack = 10u; br_part[p].mod.decay = 60u; br_part[p].mod.sustain = 70u; br_part[p].mod.release = 50u;
        br_part[p].revision += 2u;
    }
    br_fx_meter_enable(1u);
    for (int comp = 0; comp < 4; ++comp) {
        br_master.comp.source = comp == 0 ? BR_COMP_OFF : comp == 1 ? BR_COMP_ALL : comp == 2 ? BR_COMP_TRACK_1 : BR_COMP_DRUM_1;   /* DRUM_1: the kit key row is read */
        br_master.comp.threshold = 20u; br_master.comp.ratio = 4u; br_master.comp.makeup = 6u;
        br_master.revision += 2u;
        for (uint32_t p = 0; p < 4u; ++p) { set_part(p, 64u, 0u, p == 1u ? 50u : 0u, p == 2u ? 50u : 0u, 0u); br_part[p].sound.pan = 64u; }
        run(20);
        for (uint32_t k = 0; k < 4u; ++k) {
            br_part[3].sound.level = 40u + 20u * k; br_part[3].sound.gate = (k & 1u) ? 128u : 10u + 10u * k;
            br_part[3].revision += 2u; br_bpm = 90u + 10u * k;      /* kit level, gate and tempo: the cache key moves */
            br_engine_note_on(k % 3u, 50u + k, 110u);
            if (k == 2u || comp == 3) { br_engine_note_on(3u, 36u, 100u); br_engine_note_on(3u, 38u, 90u); }
            run(25);
            if (k & 2u) {                            /* an abrupt cut: mute while sounding (stale rows in the next, silent block) */
                for (uint32_t p = 0; p < 4u; ++p) { br_part[p].sound.mute = 1u; br_part[p].revision += 2u; }
                run(6);
                for (uint32_t p = 0; p < 4u; ++p) { br_part[p].sound.mute = 0u; br_part[p].revision += 2u; }
            }
            for (uint32_t p = 0; p < 4u; ++p) { br_part[p].sound.pan = (k & 1u) ? 20u + 30u * p : 64u; br_part[p].revision += 2u; }
            run(30);
            br_engine_note_off(k % 3u, 50u + k);
            run(150);
        }
        run(900);
        char name[64]; snprintf(name, sizeof name, "meter/pan/comp %d", comp);
        phase(name);
    }
    br_fx_meter_enable(0u);
    run(2000);
    /* direct injection through br_fx_process: loud block, then silent tracks next to sounding ones, with the
     * meter on and off, a pan change on a silent track, and the compressor on all sources and off. */
    {
        static int32_t bus[BR_PARTS][BR_BLOCK];
        uint32_t seed = 12345u;
        for (int comp = 0; comp < 2; ++comp)
        for (int meter = 0; meter < 2; ++meter) {
            br_fx_meter_enable((uint32_t)meter);
            br_master.comp.source = comp ? BR_COMP_ALL : BR_COMP_OFF; br_master.revision += 2u;
            for (int blk = 0; blk < 12; ++blk) {
                for (uint32_t p = 0; p < 4u; ++p) {
                    int sounding = blk % 4 == 0 ? 1 : blk % 4 == 1 ? (p == 1u) : blk % 4 == 2 ? (p == 3u) : 0;
                    for (uint32_t i = 0; i < BR_BLOCK; ++i) { seed = seed * 1664525u + 1013904223u; bus[p][i] = sounding ? (int32_t)(((int64_t)(int32_t)seed * 9000) >> 31) : 0; }
                    br_effective_sound[p].pan = (blk >= 5 && blk < 8) ? 30u + 25u * p : 64u;   /* a pan change lands on a silent track */
                }
                memset(ob, 0, sizeof ob);
                br_fx_process(bus, ob, BR_BLOCK);
                fold(&h_out, ob, sizeof ob);
                if (meter) for (uint32_t p = 0; p < 4u; ++p) { uint32_t m = br_fx_meter_read(p); fold(&h_out, &m, sizeof m); }
                fold_state();
            }
        }
        br_fx_meter_enable(0u);
        phase("direct injection");
    }
    { unsigned nz = 0; for (uint32_t p = 0; p < 4u; ++p) for (uint32_t i = 0; i < BR_BLOCK; ++i) nz += br_fx_track_stereo[p][i][0] != 0 || br_fx_track_stereo[p][i][1] != 0;
      fprintf(stderr, "[info] silent-skip track blocks %u, of which with stale non-zero rows %u; non-zero rows at end %u (mask %u)\n", silent_blocks, stale_blocks, nz, br_idle_skip_mask & 3u); } phase("final idle");
    printf("blocks %d\n", blocks_run);
    for (int i = 0; i < 9; ++i) printf("component %d %016llx\n", i, (unsigned long long)h_comp[i]);
    return 0;
}

int main(void)
{
    int fds[2][2]; pid_t pid[2];
    for (int k = 0; k < 2; ++k) {
        if (pipe(fds[k])) return 1;
        pid[k] = fork();
        if (!pid[k]) {
            br_idle_skip_mask = k ? 0u : ~0u;                /* child 0: skips on; child 1: reference */
            dup2(fds[k][1], 1); close(fds[k][0]); close(fds[k][1]);
            int rc = scenario(); fflush(stdout); _exit(rc);
        }
        close(fds[k][1]);
    }
    static char text[2][16384];
    for (int k = 0; k < 2; ++k) {
        size_t n = 0; ssize_t r;
        while ((r = read(fds[k][0], text[k] + n, sizeof text[k] - 1 - n)) > 0) n += (size_t)r;
        text[k][n] = 0; int st; waitpid(pid[k], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("child %d failed\n", k); return 1; }
    }
    fputs(text[0], stdout);
    if (strcmp(text[0], text[1])) {
        printf("MISMATCH, reference run:\n%s", text[1]);
        return 1;
    }
    printf("idle skips: output and state identical with the skips on and off\n");
    return 0;
}
