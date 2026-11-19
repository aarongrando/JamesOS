/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef IDLE_STAGE_TICKS_H
#define IDLE_STAGE_TICKS_H
/* Device-only stage stamps of one audio half (diagnostic 0xB9). Off: BR_IDLE_STAMP() is nothing. On: each
 * stamp is one fm1_ticks() read and one store; the deltas are computed once per half in the IRQ. */
/* BR_IDLE_SKIP_STREAM (default 1, device only): a half whose FX inputs are all silent, with no shared-reverb return
 * and the master at rest, does not submit the CPU1 master stream job: CPU0 runs the same finish inline (the path
 * the contained-CPU1 case already uses), skips the limiter scan when the output is provably zero and the limiter
 * is at rest, and tells the platform wrapper (br_fx_out_zero) to skip the output scale and the <<7 loop on the
 * zero buffer. Written out in instrument_fx.c br_fx_finish_block. */
#ifndef BR_IDLE_SKIP_STREAM
#define BR_IDLE_SKIP_STREAM 1
#endif
#if BR_IDLE_SKIP_STREAM && defined(FM1_DEVICE) && defined(BR_CPU1_AUDIO)
#define BR_IDLE_SKIP_STREAM_ON 1
extern volatile uint32_t br_fx_out_zero;
#define BR_IDLE_OUT_ZERO() (br_fx_out_zero)
#define BR_IDLE_OUT_ZERO_CLEAR() (br_fx_out_zero = 0u)
#else
#define BR_IDLE_SKIP_STREAM_ON 0
#define BR_IDLE_OUT_ZERO() 0u
#define BR_IDLE_OUT_ZERO_CLEAR() ((void)0)
#endif
#ifndef BR_AUDIO_TRACE
#define BR_AUDIO_TRACE 0          /* device only: a compact record per audio block in a ring (diagnostic 0xBB) */
#endif
#ifndef BR_IDLE_STAGE_TICKS
#define BR_IDLE_STAGE_TICKS BR_AUDIO_TRACE
#endif
#if BR_AUDIO_TRACE && !BR_IDLE_STAGE_TICKS
#error "BR_AUDIO_TRACE needs the stage stamps (BR_IDLE_STAGE_TICKS)"
#endif
#if BR_IDLE_STAGE_TICKS && defined(FM1_DEVICE)
#include "../hal/fm1_time.h"                     /* platform.c stamps before it includes this itself */
#define BR_IDLE_CHAIN 24u                        /* stamps 0..23: the 0xB9 chain */
/* More slots for the trace record (BR_AUDIO_TRACE): stamps from both cores, one writer per slot per block. */
enum {
    BR_TS_CPU0_MIX_END = 24,    /* CPU0: finish mix loop done, stream wait starts */
    BR_TS_CPU1_LIM0 = 25,       /* CPU1: slices done, limiter starts */
    BR_TS_CPU1_LIM1 = 26,       /* CPU1: limiter done, output scale starts */
    BR_TS_CPU1_END = 27,        /* CPU1: stream job done */
    BR_TS_CPU1_PRE = 28,        /* CPU1: pre hook done */
    BR_TS_CPU1_FLAGS = 29,      /* value: bit 0 master_active, bit 1 gr_active */
    BR_TS_ACC_WAITP = 30,       /* accumulators (ticks), cleared at br_engine_render entry: */
    BR_TS_ACC_REVERB = 31,      /*   CPU1 waiting for CPU0's produced frames, shared reverb return, */
    BR_TS_ACC_MASTER = 32,      /*   master filter/heat, */
    BR_TS_ACC_GRJOB = 33,       /*   the shared reverb's own job (br_gr_job), */
    BR_TS_ACC_C0CH1 = 34,       /*   CPU0's channel-1 share of the master low-pass (split cores) */
    BR_TS_JSTART = 36,          /* per job (16 slots each): start, source/merge end, post end, job end */
    BR_TS_JSRC = 52,
    BR_TS_JPOST = 68,
    BR_TS_JEND = 84,
    BR_TS_FILT0 = 100,          /* per part (4 slots each): track filter start/end */
    BR_TS_FILT1 = 104,
    BR_TS_ACC_DRIVE = 108,      /* per part accumulators: drive, modulation, delay, send/pan/emit stages */
    BR_TS_ACC_MOD = 112,
    BR_TS_ACC_DELAY = 116,
    BR_TS_ACC_SEND = 120
};
#define BR_IDLE_STAMPS 124u
extern volatile uint32_t br_idle_stamp[BR_IDLE_STAMPS];
#define BR_IDLE_STAMP(i) (br_idle_stamp[i] = fm1_ticks())
#define BR_IDLE_VAL(i, v) (br_idle_stamp[i] = (v))
#define BR_IDLE_NOW() fm1_ticks()
/* lap: add the ticks since `t` to accumulator i and restart the lap */
#define BR_IDLE_LAP(i, t) do { uint32_t lap_now_ = fm1_ticks(); br_idle_stamp[i] += lap_now_ - (t); (t) = lap_now_; } while (0)
#define BR_IDLE_JOB(job) ((uint32_t)((job) - br_audio_source_jobs) & 15u)
#define BR_IDLE_CLEAR_ACC() do { uint32_t acc_; for (acc_ = BR_TS_ACC_WAITP; acc_ <= BR_TS_ACC_C0CH1; ++acc_) br_idle_stamp[acc_] = 0u; \
    for (acc_ = BR_TS_ACC_DRIVE; acc_ < BR_IDLE_STAMPS; ++acc_) br_idle_stamp[acc_] = 0u; } while (0)
#if BR_AUDIO_TRACE
uint32_t br_audio_trace_commit(uint32_t seq, uint32_t render_ticks, uint32_t ui_lcd_bytes, uint32_t ui_lcd_ticks);
uint32_t br_audio_trace_read(uint32_t after, uint8_t *out);
#endif
#else
#define BR_IDLE_STAMP(i) ((void)0)
#define BR_IDLE_VAL(i, v) ((void)0)
#define BR_IDLE_NOW() 0u
#define BR_IDLE_LAP(i, t) ((void)(t))
#define BR_IDLE_JOB(job) 0u
#define BR_IDLE_CLEAR_ACC() ((void)0)
#endif
/* Stamp numbers, in execution order on CPU0 (the word of the 0xB9 reply that starts at the stamp is the same
 * number):
 *  0 window start (platform_audio_render_half)          12 just before the CPU1 source submit
 *  1 br_engine_render entry                             13 CPU0's own source queue share done
 *  2 after seq clock + br_comp_key clear                14 CPU1 source wait returned
 *  3 after the first snapshot/select/build loop         15 end of join validation
 *  4 after clock project + take_events (+deferred)      16 after fused osc copy / post-join record
 *  5 after pattern irq/host begin + transport branch    17 br_fx_process_ram entry
 *  6 after arp x3 + seq x4                              18 just before the stream submit
 *  7 after second build/select pass + mono limit        19 after the stream wait returned
 *  8 after smooth_sends + level + comp snap + kit ctl   20 after commit
 *  9 after br_fx_fusion_prepare                         21 window end (after the <<7 loop)
 * 10 after br_audio_build_fusion_jobs                   22 CPU1: stream job picked up
 * 11 after job init + fixed plan prepare (pre-publish)  23 CPU1: after pre hook + master begin_block */
#endif
