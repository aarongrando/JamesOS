/* Host probe of the idle audio block (a tool, not a test; asserts nothing about timings).
 *
 * Boots the default session (br_app_init, as tests/test_master_silence.c does), lets everything decay, then
 * times N idle blocks. The stages are timed by calling the firmware's own static stage functions directly
 * (the file is a unity include of dinghy.c), in the order br_engine_render / br_fx_process run them, so no
 * firmware edit is needed. The whole-block time (platform_audio_render_half, the code the 0x60 metric
 * brackets) is measured first. The host build has no BR_SPLIT_AUDIO: no job queue, no CPU1 hand-off, the
 * master stage inline in br_fx_finish_block (the same stage code the device runs on CPU1).
 * HOST NUMBERS ARE RELATIVE ONLY: one core runs everything the unit spreads over two, -O2 not -O3/-Oz,
 * a desktop clock, no flash/RAM-code split. Build and run with tests/probe_idle_stages.py. */
#define DINGHY_HOST 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include <time.h>
#include <stdio.h>

static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static volatile int32_t sink;
static int32_t ob[BR_BLOCK * 2u];

static void render_whole(void)   /* the window the 0x60 metric brackets: clear, app_audio, commit, <<7 */
{
    uint32_t r = platform_audio_render_half(ob, BR_BLOCK);
    if (r != FM1_AUDIO_RENDER_OK) { fprintf(stderr, "render_half returned %u\n", r); exit(2); }
}

static int part_quiet_all(void) { for (uint32_t p = 0; p < BR_PARTS; ++p) if (!br_quiet_part[p]) return 0; return 1; }

static void dump_state(const char *tag)
{
    printf("-- state %s\n", tag);
    for (uint32_t p = 0; p < BR_PARTS; ++p) {
        const volatile br_sound_t *s = &br_part[p].sound;
        printf("   part %u: mode=%u preset=%u quiet=%u env.stage=%u filter=%u res=%u keytrack=%u mix=%d ladder_s=[%d %d %d %d] low=%d band=%d "
               "drive=%u delay=%u reverb=%u mod_mix=%u pan=%u tail_samples=%u tail_storage=%u drive_send=%d pan_prev=%x\n",
               p, br_mode[p], s->preset, (unsigned)br_quiet_part[p], br_env[p].stage, s->filter, s->filter_resonance, s->filter_keytrack,
               br_filter[p].mix, br_filter[p].ladder_s[0], br_filter[p].ladder_s[1], br_filter[p].ladder_s[2], br_filter[p].ladder_s[3],
               br_filter[p].low, br_filter[p].band, s->drive, s->delay, s->reverb, br_part[p].fx.mod_mix, s->pan,
               br_fx_track[p].tail_samples, br_fx_track[p].tail_storage, (int)br_fx_track[p].drive_send, (unsigned)br_fx_pan_prev[p]);
    }
    printf("   master: filter=%u res=%u heat_type=%u heat_amount=%u comp.source=%u (OFF=%u) reverb_engine=%u size=%u decay=%u\n",
           br_master.fx.filter, br_master.fx.resonance, br_master.fx.heat_type, br_master.fx.heat_amount,
           br_master.comp.source, (unsigned)BR_COMP_OFF, br_master.reverb_engine, br_master.reverb_size, br_master.reverb_decay);
    printf("   gr: run=%u rv.idle=%u rv.configured=%u | master_state: filter_tail=%u wow_tail=%u limit_cut=%u | comp bypassed=%u\n",
           (unsigned)br_gr_run, (unsigned)br_gr_rv.idle, (unsigned)br_gr_rv.configured, (unsigned)br_master_state.filter_tail,
           (unsigned)br_master_state.wow_tail, (unsigned)br_master_state.limit_cut, (unsigned)br_comp_bypassed());
    printf("   br_playing=%u quiet_all=%u br_fx_quiet=%d br_engine_quiet=%d\n", (unsigned)br_playing, part_quiet_all(),
           (int)br_fx_quiet(), (int)br_engine_quiet());
}

static int all_zero(const int32_t *a, uint32_t n) { for (uint32_t i = 0; i < n; ++i) if (a[i]) return 0; return 1; }

/* Per-stage timing: each lambda-like block below runs N times; ns per block is reported. */
#define STAGE_BEGIN(name) { const char *stage_name = name; double acc = 0; for (int b = 0; b < N; ++b) { double t0 = now_ns();
#define STAGE_END() acc += now_ns() - t0; } report(stage_name, acc / N); }
static double clock_overhead;
static double total_ns;
static void report(const char *name, double ns)
{
    ns -= clock_overhead; if (ns < 0) ns = 0;
    printf("   %-62s %9.0f ns  %6.2f %%\n", name, ns, total_ns > 0 ? 100.0 * ns / total_ns : 0.0);
}

static double time_whole(int N)
{
    for (int w = 0; w < 200; ++w) render_whole();
    double best = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        double t0 = now_ns();
        for (int b = 0; b < N; ++b) render_whole();
        double ns = (now_ns() - t0) / N;
        if (ns < best) best = ns;
    }
    return best;
}

int main(int argc, char **argv)
{
    int N = argc > 1 ? atoi(argv[1]) : 3000;
    int decay_blocks = argc > 2 ? atoi(argv[2]) : 172 * 12;    /* 12 s of blocks after the note */
    int scenario_note = argc > 3 ? atoi(argv[3]) : 0;           /* 1: play and release a note on every part first */
    int variant = argc > 4 ? atoi(argv[4]) : 0;                 /* 1: every track filter neutral; 2: also no sends */
    br_app_init(); fm1_ms = 1000u;
    br_master.revision++; RING_PUBLISH();
    if (variant) {
        for (uint32_t p = 0; p < BR_PARTS; ++p) {
            br_part[p].sound.filter = 64u; br_part[p].sound.filter_resonance = 0u;
            if (variant == 3) { br_part[p].sound.delay = 90u; br_part[p].sound.reverb = 90u; br_part[p].sound.drive = 60u; br_part[p].fx.mod_mix = 64u; br_part[p].fx.mod_depth = 64u; br_part[p].fx.delay_feedback = 100u; }
            else if (variant == 4) { br_part[p].sound.delay = 40u; br_part[p].sound.reverb = 40u; br_part[p].sound.drive = 30u; br_part[p].fx.mod_mix = 32u; br_part[p].fx.mod_depth = 64u; }
            else if (variant >= 2) { br_part[p].sound.delay = 0u; br_part[p].sound.reverb = 0u; br_part[p].sound.drive = 0u; }
            br_part[p].revision += 2u;
        }
        for (int b = 0; b < 8; ++b) render_whole();
    }
    if (scenario_note) {
        for (uint32_t p = 0; p < BR_PARTS; ++p) br_engine_note_on(p, 60u + p, 110u);
        for (int b = 0; b < 100; ++b) render_whole();
        for (uint32_t p = 0; p < BR_PARTS; ++p) br_engine_note_off(p, 60u + p);
    }
    int settled_at = -1, rv_idle_at = -1, quiet_at = -1, zero_out_at = -1;
    int q_at[BR_PARTS], tail_at[BR_PARTS], filt_at[BR_PARTS], silent_at[BR_PARTS], env_at[BR_PARTS];
    int gr_run_off_at = -1, mf_tail_at = -1, drive_at[BR_PARTS];
    for (uint32_t p = 0; p < BR_PARTS; ++p) q_at[p] = tail_at[p] = filt_at[p] = silent_at[p] = env_at[p] = drive_at[p] = -1;
    int last_nonzero_out = -1;
    static double bucket_ns[512]; static int bucket_n[512];
    for (int b = 0; b < decay_blocks; ++b) {
        double tb = now_ns();
        render_whole();
        { int k = b / 43; if (k < 512) { bucket_ns[k] += now_ns() - tb; ++bucket_n[k]; } }
        if (!all_zero(ob, BR_BLOCK * 2u)) last_nonzero_out = b;
        if (b % 250 == 0 || b == decay_blocks - 1) {
            int32_t pk = 0; for (uint32_t i = 0; i < 2u * BR_BLOCK; ++i) { int32_t v = ob[i] < 0 ? -ob[i] : ob[i]; if (v > pk) pk = v; }
            printf("   [trace] block %4d: out peak(<<7 scaled) %d | tail_samples %u/%u/%u/%u | gr idle %u | drive_send %d/%d/%d/%d\n", b, pk,
                   br_fx_track[0].tail_samples, br_fx_track[1].tail_samples, br_fx_track[2].tail_samples, br_fx_track[3].tail_samples,
                   (unsigned)br_gr_rv.idle, (int)br_fx_track[0].drive_send, (int)br_fx_track[1].drive_send, (int)br_fx_track[2].drive_send, (int)br_fx_track[3].drive_send);
        }
        if (quiet_at < 0 && part_quiet_all()) quiet_at = b;
        if (rv_idle_at < 0 && br_gr_rv.idle) rv_idle_at = b;
        if (gr_run_off_at < 0 && !br_gr_run) gr_run_off_at = b;
        if (mf_tail_at < 0 && !br_master_state.filter_tail) mf_tail_at = b;
        if (zero_out_at < 0 && all_zero(ob, BR_BLOCK * 2u)) zero_out_at = b;
        if (settled_at < 0 && br_fx_quiet() && br_engine_quiet()) settled_at = b;
        for (uint32_t p = 0; p < BR_PARTS; ++p) {
            const br_filter_t *f = &br_filter[p];
            int fz = !f->low && !f->band && !f->ladder_s[0] && !f->ladder_s[1] && !f->ladder_s[2] && !f->ladder_s[3] && !f->extra_s[0] && !f->extra_s[1];
            if (q_at[p] < 0 && br_quiet_part[p]) q_at[p] = b;
            if (tail_at[p] < 0 && !br_fx_track[p].tail_samples) tail_at[p] = b;
            if (filt_at[p] < 0 && fz) filt_at[p] = b;
            if (env_at[p] < 0 && br_env[p].stage == BR_ENV_OFF) env_at[p] = b;
            if (silent_at[p] < 0 && br_fx_track_silent_seq[p] == br_fx_block_seq) silent_at[p] = b;
            if (drive_at[p] < 0 && !br_fx_track[p].drive_send) drive_at[p] = b;
        }
    }
    printf("   (block index of the FIRST block where each condition held; -1 = never within the run)\n");
    for (uint32_t p = 0; p < BR_PARTS; ++p)
        printf("   part %u: env OFF %d | br_quiet_part %d | track filter states exactly 0 %d | fx tail_samples==0 %d | fx track silent-skip taken %d | drive_send==0 %d\n",
               p, env_at[p], q_at[p], filt_at[p], tail_at[p], silent_at[p], drive_at[p]);
    printf("   whole-block host ns, averaged per 0.25 s of decay (block time vs time since release):\n   ");
    for (int k = 0; k < 512 && bucket_n[k]; ++k) printf("%.2fs:%.0f ", k * 0.25, bucket_ns[k] / bucket_n[k]);
    printf("\n");
    printf("   shared reverb: br_gr_run==0 at %d; master filter_tail==0 at %d; last block with non-zero output: %d\n", gr_run_off_at, mf_tail_at, last_nonzero_out);
    printf("== scenario: %s, variant %d, %d decay blocks (%.1f s) ==\n", scenario_note ? "note on every part, then release" : "pristine boot", variant,
           decay_blocks, decay_blocks * 256.0 / 44100.0);
    printf("   first block index with: all br_quiet_part=1: %d | shared reverb idle: %d | all-zero output: %d | br_fx_quiet&&br_engine_quiet: %d\n",
           quiet_at, rv_idle_at, zero_out_at, settled_at);
    dump_state("after decay");

    /* ---- whole block ---- */
    double serial = time_whole(N);
    printf("== whole idle block (platform_audio_render_half), host ns per 256-frame block ==\n");
    printf("   serial host path (br_engine_render, then br_fx_process; no job queue, no second core): %9.0f ns\n", serial);
    total_ns = serial;
    {   /* clock-call overhead */
        double t0 = now_ns(); for (int i = 0; i < 100000; ++i) (void)now_ns(); clock_overhead = (now_ns() - t0) / 100000.0;
        printf("   (clock call overhead %.0f ns, subtracted from each stage)\n", clock_overhead);
    }
    for (int w = 0; w < 100; ++w) render_whole();

    static int32_t bus_local[BR_PARTS][BR_BLOCK];
    uint32_t n = BR_BLOCK;
    printf("== stage split (percent of the fusion-off whole block), engine side, direct calls ==\n");
    STAGE_BEGIN("platform wrapper: clear 512 words")        for (uint32_t i = 0; i < 2u * n; ++i) ob[i] = 0; sink += ob[3]; STAGE_END()
    STAGE_BEGIN("platform wrapper: out <<= 7, 512 words")   for (uint32_t i = 0; i < 2u * n; ++i) ob[i] <<= 7; sink += ob[3]; STAGE_END()
    STAGE_BEGIN("platform_audio_block_done (scale_q12_clip 512)") platform_audio_block_done(ob, n); STAGE_END()
    STAGE_BEGIN("br_seq_clock += n; br_comp_key clear 4x256")
        for (uint32_t p = 0; p < BR_PARTS; ++p) { br_seq_clock[p] += n; for (uint32_t i = 0; i < BR_BLOCK; ++i) br_comp_key[p][i] = 0; }
    STAGE_END()
    STAGE_BEGIN("br_snapshot+select_mode+build_effective x4 (pass 1)")
        for (uint32_t p = 0; p < BR_PARTS; ++p) { br_snapshot(p); br_select_mode(p); br_build_effective(p); }
    STAGE_END()
    STAGE_BEGIN("br_take_events + br_clock_project + pattern irq hooks")
        br_take_events(); br_clock_project();
        br_pattern_irq_normalize_internal(); br_pattern_irq_check_external_gap(); br_pattern_host_begin_block();
    STAGE_END()
    STAGE_BEGIN("br_arp_process x3 + br_seq_process x4")
        for (uint32_t p = 0; p < BR_SYNTH_PARTS; ++p) br_arp_process(p, n);
        for (uint32_t p = 0; p < BR_PARTS; ++p) br_seq_process(p, n);
    STAGE_END()
    STAGE_BEGIN("build_effective+select_mode x4 (pass 2), mono_limit, smooth_sends, level_prepare x4, comp snapshot, kit controls")
        for (uint32_t p = 0; p < BR_PARTS; ++p) { br_build_effective(p); br_select_mode(p); }
        br_model_mono_limit(); br_smooth_sends();
        for (uint32_t p = 0; p < BR_PARTS; ++p)
            br_effective_sound[p].level = br_level_prepare(&br_level_slew[p], &br_level_block[p], br_level_law(br_level_raw[p]), n, FS / 200u);
        br_snapshot_source_compressor(); br_prepare_kit_source_controls();
    STAGE_END()
    for (uint32_t p = 0; p < BR_PARTS; ++p) {
        char name[96];
        if (br_mode[p] == 1u || br_mode[p] == 4u) {
            snprintf(name, sizeof name, "part %u source (kit, mode %u): 4 lane jobs", p, br_mode[p]);
            STAGE_BEGIN(name)
                for (uint32_t lane = 0; lane < BR_DRUM_LANES; ++lane)
                    br_engine_source_part(p, br_audio_kit_partial[0][lane], br_audio_kit_key_output(p, lane, 1u << lane), n, 1u << lane);
            STAGE_END()
            snprintf(name, sizeof name, "part %u kit finalize (4-lane merge, replicated loop)", p);
            STAGE_BEGIN(name)
                for (uint32_t i = 0; i < n; ++i) { int32_t s = 0, k = 0; for (uint32_t l = 0; l < 4u; ++l) { s += br_audio_kit_partial[0][l][i]; const int32_t *kr = br_audio_kit_key_output(p, l, 1u << l); if (kr) k += kr[i]; } bus_local[p][i] = s; br_comp_key[p][i] = k; }
            STAGE_END()
        } else {
            snprintf(name, sizeof name, "part %u source (synth, mode %u)", p, br_mode[p]);
            STAGE_BEGIN(name) br_engine_source_part(p, bus_local[p], 0, n, BR_DRUM_ALL_MASK); STAGE_END()
        }
        snprintf(name, sizeof name, "part %u post row (env, level, track filter, quiet test)", p);
        STAGE_BEGIN(name) sink += br_engine_post_part_row(p, bus_local[p], br_comp_key[p], n); STAGE_END()
    }

    printf("== stage split, FX side (br_fx_process in pieces) ==\n");
    STAGE_BEGIN("br_fx_prepare_block (comp snapshot, gr_plan, any_enabled, ctrl cache)") br_fx_job_n = n; br_fx_prepare_block(n); STAGE_END()
    br_fx_job_bus = bus_local;
    for (uint32_t p = 0; p < BR_PARTS; ++p) {
        char name[96];
        snprintf(name, sizeof name, "br_fx_process_track_job(%u) (silent-track skip path; p=%u hosts gr_job)", p, BR_GR_HOST);
        STAGE_BEGIN(name) br_fx_process_track_job(p); STAGE_END()
    }
    STAGE_BEGIN("br_fx_finish_inputs_silent (scan 4x256 tracks, comp at rest)")
        sink += (int32_t)br_fx_finish_inputs_silent(bus_local, n, br_fx_comp_active, br_fx_dry_center);
    STAGE_END()
    STAGE_BEGIN("br_master_fx_begin_block (snapshot, lfo, gf_apply, neutral test)") sink += (int32_t)br_master_fx_begin_block(n); STAGE_END()
    STAGE_BEGIN("br_gr_add_range 0..256") br_gr_add_range(ob, 0u, n); STAGE_END()
    STAGE_BEGIN("br_master_limit_block (peak scan 512 words)") memset(ob, 0, sizeof ob); br_master_limit_block(ob, n); STAGE_END()
    STAGE_BEGIN("(device CPU1) br_fx_scale_q12_clip_range 512 words, gain 4096") br_fx_scale_q12_clip_range(ob, 2u * n, 4096u); STAGE_END()
    STAGE_BEGIN("br_engine_post_join_run (4x256 bus sum, global LFO, mod capture/advance x4; CPU1 'pre' hook)")
        br_post_join.bus = bus_local; br_post_join.n = n; br_post_join.pending = 1u; br_engine_post_join_run();
    STAGE_END()
    STAGE_BEGIN("br_audio_kit_meter_capture") br_audio_kit_meter_capture(n, br_mode[BR_DRUM_PART]); STAGE_END()
    STAGE_BEGIN("br_punch_process (effect 0)")
        br_punch_process(&br_punch_dsp, &br_punch_cfg, 0u, ob, n, FS * 60u / 120u);
    STAGE_END()
    printf("   (the sum of the rows above is compared with the whole-block time by the runner)\n");
    /* ---- does the track filter reach an exact all-zero state after silence, for every control/resonance? ---- */
    {
        uint32_t slow_over_1s = 0u, never = 0u, total = 0u, worst = 0u, worst_c = 0u, worst_r = 0u, bad_fixed = 0u;
        for (uint32_t control = 0; control < 128u; ++control) {
            if (control == 63u || control == 64u) continue;
            for (uint32_t res = 0; res < 128u; res += (res < 16u ? 1u : 7u)) {
                br_filter_t f; static int32_t row[256]; uint32_t seed = control * 977u + res * 31u + 5u;
                br_filter_init(&f);
                for (int k = 0; k < 40; ++k) {                       /* settle the mix, excite with loud noise */
                    for (int i = 0; i < 256; ++i) { seed = seed * 1664525u + 1013904223u; row[i] = (int32_t)(((int64_t)(int32_t)seed * 20000) >> 31); }
                    br_filter_block(&f, row, 256u, control, res);
                }
                uint32_t blocks = 0u;
                for (; blocks < 20000u; ++blocks) {                  /* up to 20000 blocks = 116 s of exact silence */
                    int fz = !f.low && !f.band && !f.ladder_s[0] && !f.ladder_s[1] && !f.ladder_s[2] && !f.ladder_s[3] && !f.extra_s[0] && !f.extra_s[1];
                    if (fz) break;
                    memset(row, 0, sizeof row);
                    br_filter_block(&f, row, 256u, control, res);
                }
                ++total;
                if (blocks >= 20000u) { ++never; printf("   never settles: control %u res %u (low=%d band=%d s=[%d %d %d %d] e=[%d %d])\n", control, res, f.low, f.band, f.ladder_s[0], f.ladder_s[1], f.ladder_s[2], f.ladder_s[3], f.extra_s[0], f.extra_s[1]); continue; }
                if (blocks > 172u) ++slow_over_1s;
                if (blocks > worst) { worst = blocks; worst_c = control; worst_r = res; }
                {   /* the fixed point: zero in, zero state -> zero out and the same state words */
                    br_filter_t g = f; memset(row, 0, sizeof row);
                    br_filter_block(&g, row, 256u, control, res);
                    if (!all_zero(row, 256u) || g.low || g.band || g.ladder_s[0] || g.ladder_s[1] || g.ladder_s[2] || g.ladder_s[3] || g.extra_s[0] || g.extra_s[1]) ++bad_fixed;
                }
            }
        }
        printf("== track filter: exact zero state after silence (all controls except 63/64, resonances 0..15 then every 7th) ==\n");
        printf("   combinations %u; never reached all-zero within 20000 silent blocks (116 s): %u; settling took over 1 s: %u; slowest %u blocks (control %u res %u); zero-state fixed-point violations: %u\n",
               total, never, slow_over_1s, worst, worst_c, worst_r, bad_fixed);
    }
    return 0;
}
