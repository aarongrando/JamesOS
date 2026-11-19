/* SPDX-License-Identifier: GPL-3.0-only */
/* BR_AUDIO_TRACE: one compact record per audio block in a 64-record ring (ordinary .bss), read with
 * diagnostic 0xBB. Included at the end of instrument_audio_unit.c, so the stamps, jobs and FX state are
 * all visible. Flash code only. The record is written by the audio IRQ (commit) after the half is done and
 * read from the main loop (read) with a sequence check; no stamp is taken here.
 *
 * Record, 124 bytes, little-endian. T = u16 of ticks >> 2 (saturating), M = u8 of ticks >> 6 (saturating at
 * 16,320 ticks; any saturated M sets flag bit 14). Offsets:
 *   0 u32 sequence (the audio half counter; gaps are halves that were dropped late)
 *   4 T   whole window (platform_audio_render_half)
 *   6 u16 flags: 0 playing, 1..4 parts 0..3 NOT quiet, 5 master active, 6 shared reverb running,
 *         7 compressor active, 8 CPU1 stream job used, 9 window over budget, 10 worker fault,
 *         11 source/FX fusion active, 12 FX dry-centre, 13 split graph (more than 8 jobs),
 *         14 an M field saturated, 15 FX meter enabled
 *   8 u16 jobs: bit j (0..11) = job j ran on CPU1; bits 12..15 = job count (max 15)
 *  10 i16 block period error: (stamp0 - previous stamp0 - half budget) >> 2, saturating (IRQ entry jitter)
 *  12 u16 LCD bytes sent since the previous block (BR_UI_TIMING_DIAG builds, else 0)
 *  14 T   LCD ticks (waiting on / starting transfers) since the previous block (likewise)
 *  16 M x12  CPU0 control spans: start->engine entry, comp_key clear, first build loop, clock+events,
 *            pattern+transport, arp+seq, second build pass, sends/level/comp/kit controls, FX prepare,
 *            build jobs, cost hints+job init+fixed plan, publish
 *  28 T   CPU1 submit + CPU0 source share        30 T  wait for CPU1 (join)
 *  32 T   join validation
 *  34 M   osc copy + post-join record   35 M render return -> FX entry   36 M FX entry -> stream submit
 *  37 M   commit                        38 M tail (the <<7 loop)
 *  39 T   CPU0 finish mix loop + compressor (stream submit -> wait starts)    41 T CPU0 pure wait
 *  43 M   CPU1 stream pickup latency    44 M pre hook (post-join modulation)  45 M master begin_block
 *  46 T   shared reverb return (br_gr_add_range, summed over slices)   48 T master filter/heat (summed)
 *  50 T   CPU1 waiting for CPU0's produced frames (summed)
 *  52 M   limiter    53 M output scale
 *  54 8 x (T start offset from window start, T duration): jobs 0..7 (synth tracks 0..2, kit lanes 3..6, finalize 7)
 *  86 4 x 6 M per part 0..3: post env+level, track filter, drive, modulation, delay, send/pan/emit
 * 110 4 x T: source time of jobs 0, 1, 2 and merge time of job 7
 * 118 T   start of job 11 (shared reverb as its own job, split graph; 0 otherwise)  120 M its duration
 * 121 T   time in br_gr_job (the shared reverb engine, wherever it runs; summed)
 * 123 M   CPU0 channel-1 share of the master low-pass (BR_MASTER_SPLIT_CORES; summed over slices; 0 when not split)
 */
#if BR_AUDIO_TRACE && defined(FM1_DEVICE)
#define BR_TRACE_BYTES 124u
#define BR_TRACE_SLOTS 64u
static volatile uint8_t br_trace_ring[BR_TRACE_SLOTS][BR_TRACE_BYTES];
static volatile uint32_t br_trace_seq[BR_TRACE_SLOTS];
static uint32_t br_trace_prev_stamp0, br_trace_prev_bytes, br_trace_prev_lcd, br_trace_have_prev;
static uint32_t br_trace_sat;

static uint32_t br_trace_t(uint32_t ticks) { ticks >>= 2; return ticks > 65535u ? 65535u : ticks; }
static uint32_t br_trace_m(uint32_t ticks)
{
    ticks >>= 6;
    if (ticks > 255u) { br_trace_sat = 1u; return 255u; }
    return ticks;
}
static void br_trace_put16(volatile uint8_t *r, uint32_t at, uint32_t v) { r[at] = (uint8_t)v; r[at + 1u] = (uint8_t)(v >> 8); }

uint32_t br_audio_trace_commit(uint32_t seq, uint32_t render_ticks, uint32_t ui_lcd_bytes, uint32_t ui_lcd_ticks)
{
    volatile uint32_t *const s = br_idle_stamp;      /* read in place: no 500-byte copy on the IRQ stack */
    uint32_t i, p, j, flags = 0u, mask = 0u, count;
    volatile uint8_t *r;
    uint32_t slot = seq % BR_TRACE_SLOTS;
    int32_t period;
    br_trace_sat = 0u;
    count = br_audio_source_queue.count;
    if (count > 15u) count = 15u;
    r = br_trace_ring[slot];
    br_trace_seq[slot] = 0u;                          /* in progress */
    for (i = 0u; i < BR_TRACE_BYTES; ++i) r[i] = 0u;
    for (j = 0u; j < count && j < 12u; ++j)
        if (br_audio_source_jobs[j].executor_core == 1u) mask |= 1u << j;
    flags = (br_playing ? 1u : 0u);
    for (p = 0u; p < BR_PARTS; ++p) if (!br_quiet_part[p]) flags |= 2u << p;
    flags |= ((s[BR_TS_CPU1_FLAGS] & 1u) ? 1u << 5 : 0u) | (br_gr_run ? 1u << 6 : 0u) |
             (br_fx_comp_active ? 1u << 7 : 0u) | (s[22] ? 1u << 8 : 0u) |
             (render_ticks > FM1_AUDIO_HALF_TICKS ? 1u << 9 : 0u) | (br_audio_source_worker_fault ? 1u << 10 : 0u) |
             (br_audio_source_fx_fusion_active ? 1u << 11 : 0u) | (br_fx_dry_center ? 1u << 12 : 0u) |
             (count > 8u ? 1u << 13 : 0u) | (br_fx_meter_enabled ? 1u << 15 : 0u);
    r[0] = (uint8_t)seq; r[1] = (uint8_t)(seq >> 8); r[2] = (uint8_t)(seq >> 16); r[3] = (uint8_t)(seq >> 24);
    br_trace_put16(r, 4u, br_trace_t(s[21] - s[0]));
    br_trace_put16(r, 8u, mask | (count << 12));
    period = br_trace_have_prev ? (int32_t)(s[0] - br_trace_prev_stamp0) - (int32_t)FM1_AUDIO_HALF_TICKS : 0;
    period >>= 2;
    if (period > 32767) period = 32767;
    if (period < -32767) period = -32767;
    br_trace_put16(r, 10u, (uint32_t)period);
    br_trace_put16(r, 12u, br_trace_have_prev ? (ui_lcd_bytes - br_trace_prev_bytes > 65535u ? 65535u : ui_lcd_bytes - br_trace_prev_bytes) : 0u);
    br_trace_put16(r, 14u, br_trace_have_prev ? br_trace_t(ui_lcd_ticks - br_trace_prev_lcd) : 0u);
    br_trace_prev_stamp0 = s[0]; br_trace_prev_bytes = ui_lcd_bytes; br_trace_prev_lcd = ui_lcd_ticks; br_trace_have_prev = 1u;
    for (i = 0u; i < 12u; ++i) r[16u + i] = (uint8_t)br_trace_m(s[i + 1u] - s[i]);
    br_trace_put16(r, 28u, br_trace_t(s[13] - s[12]));
    br_trace_put16(r, 30u, br_trace_t(s[14] - s[13]));
    br_trace_put16(r, 32u, br_trace_t(s[15] - s[14]));
    r[34] = (uint8_t)br_trace_m(s[16] - s[15]);
    r[35] = (uint8_t)br_trace_m(s[17] - s[16]);
    r[36] = (uint8_t)br_trace_m(s[18] - s[17]);
    r[37] = (uint8_t)br_trace_m(s[20] - s[19]);
    r[38] = (uint8_t)br_trace_m(s[21] - s[20]);
    br_trace_put16(r, 39u, br_trace_t(s[BR_TS_CPU0_MIX_END] - s[18]));
    br_trace_put16(r, 41u, br_trace_t(s[19] - s[BR_TS_CPU0_MIX_END]));
    if (s[22]) {                                      /* a stream job ran: its CPU1 stamps are valid */
        r[43] = (uint8_t)br_trace_m(s[22] - s[18]);
        r[44] = (uint8_t)br_trace_m(s[BR_TS_CPU1_PRE] - s[22]);
        r[45] = (uint8_t)br_trace_m(s[23] - s[BR_TS_CPU1_PRE]);
        br_trace_put16(r, 46u, br_trace_t(s[BR_TS_ACC_REVERB]));
        br_trace_put16(r, 48u, br_trace_t(s[BR_TS_ACC_MASTER]));
        br_trace_put16(r, 50u, br_trace_t(s[BR_TS_ACC_WAITP]));
        r[52] = (uint8_t)br_trace_m(s[BR_TS_CPU1_LIM1] - s[BR_TS_CPU1_LIM0]);
        r[53] = (uint8_t)br_trace_m(s[BR_TS_CPU1_END] - s[BR_TS_CPU1_LIM1]);
    }
    for (j = 0u; j < 8u && j < count; ++j) {
        br_trace_put16(r, 54u + 4u * j, br_trace_t(s[BR_TS_JSTART + j] - s[0]));
        br_trace_put16(r, 56u + 4u * j, br_trace_t(s[BR_TS_JEND + j] - s[BR_TS_JSTART + j]));
    }
    for (p = 0u; p < BR_PARTS; ++p) {
        /* post row = the job's post end minus its source end, for the job that carries this part */
        uint32_t post = 0u, filt = s[BR_TS_FILT1 + p] - s[BR_TS_FILT0 + p];
        for (j = 0u; j < count && j < 8u; ++j)
            if (br_audio_source_jobs[j].part == p && s[BR_TS_JPOST + j] - s[BR_TS_JSRC + j] < 1000000u &&
                (br_audio_source_jobs[j].kind == BR_AUDIO_JOB_FUSED_TRACK ||
                 br_audio_source_jobs[j].kind == BR_AUDIO_JOB_TRACK_SOURCE ||
                 br_audio_source_jobs[j].kind == BR_AUDIO_JOB_KIT_FINALIZE)) {
                post = s[BR_TS_JPOST + j] - s[BR_TS_JSRC + j];
                break;
            }
        post = post > filt ? post - filt : 0u;
        r[86u + 6u * p] = (uint8_t)br_trace_m(post);
        r[87u + 6u * p] = (uint8_t)br_trace_m(filt);
        r[88u + 6u * p] = (uint8_t)br_trace_m(s[BR_TS_ACC_DRIVE + p]);
        r[89u + 6u * p] = (uint8_t)br_trace_m(s[BR_TS_ACC_MOD + p]);
        r[90u + 6u * p] = (uint8_t)br_trace_m(s[BR_TS_ACC_DELAY + p]);
        r[91u + 6u * p] = (uint8_t)br_trace_m(s[BR_TS_ACC_SEND + p]);
    }
    for (j = 0u; j < 3u && j < count; ++j) br_trace_put16(r, 110u + 2u * j, br_trace_t(s[BR_TS_JSRC + j] - s[BR_TS_JSTART + j]));
    if (count > 7u) br_trace_put16(r, 116u, br_trace_t(s[BR_TS_JSRC + 7u] - s[BR_TS_JSTART + 7u]));
    if (count > 11u) {
        br_trace_put16(r, 118u, br_trace_t(s[BR_TS_JSTART + 11u] - s[0]));
        r[120] = (uint8_t)br_trace_m(s[BR_TS_JEND + 11u] - s[BR_TS_JSTART + 11u]);
    }
    br_trace_put16(r, 121u, br_trace_t(s[BR_TS_ACC_GRJOB]));
    r[123] = (uint8_t)br_trace_m(s[BR_TS_ACC_C0CH1]);
    if (br_trace_sat) flags |= 1u << 14;
    br_trace_put16(r, 6u, flags);
    br_idle_stamp[BR_TS_CPU1_LIM0] = br_idle_stamp[BR_TS_CPU1_LIM1] = br_idle_stamp[BR_TS_CPU1_END] =
        br_idle_stamp[BR_TS_CPU1_PRE] = br_idle_stamp[BR_TS_CPU1_FLAGS] = 0u;   /* 22 and 23: br_idle_stage_finish */
    br_trace_seq[slot] = seq;                         /* published last */
    return 1u;
}

/* The oldest record with a sequence above `after` (0: the oldest there is); 0 if none yet. Copies 124 bytes. */
uint32_t br_audio_trace_read(uint32_t after, uint8_t *out)
{
    uint32_t slot, best = 0u, best_slot = 0u, tries, i, seq;
    for (tries = 0u; tries < 3u; ++tries) {
        best = 0u;
        for (slot = 0u; slot < BR_TRACE_SLOTS; ++slot) {
            seq = br_trace_seq[slot];
            if (seq && seq > after && (!best || seq < best)) { best = seq; best_slot = slot; }
        }
        if (!best) return 0u;
        for (i = 0u; i < BR_TRACE_BYTES; ++i) out[i] = br_trace_ring[best_slot][i];
        if (br_trace_seq[best_slot] == best) return BR_TRACE_BYTES;   /* not overwritten while copying */
    }
    return 0u;
}
#endif
