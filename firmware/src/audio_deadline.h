/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef BR_AUDIO_DEADLINE_H
#define BR_AUDIO_DEADLINE_H
#include "../hal/fm1_audio_rate.h"

/* Fixed T4 budget for one 256-frame half, measured with the existing floor
 * conversion. Reserve twice the observed maximum post-final-worker-wait-to-ack tail. */
#define BR_AUDIO_HALF_DEADLINE_TICKS FM1_AUDIO_HALF_TICKS
#if FM1_NATIVE_OUTPUT_32K
#define BR_AUDIO_PIPELINE_TAIL_RESERVE_TICKS FM1_NATIVE32_TAIL_RESERVE_TICKS
#else
#define BR_AUDIO_PIPELINE_TAIL_RESERVE_TICKS 9216u
#endif
#define BR_AUDIO_PIPELINE_SOURCE_BUDGET_TICKS \
    (BR_AUDIO_HALF_DEADLINE_TICKS - BR_AUDIO_PIPELINE_TAIL_RESERVE_TICKS)

/* The HANG limit: how long CPU0 waits for a CPU1 that has accepted a job before it takes CPU1 for hung and
 * holds it in reset (for good: CPU1 is never restarted at runtime). It is one half after the half's source
 * deadline, about 269,000 ticks (1.93 half periods, 11.2 ms) from the render start. A CPU1 that is merely
 * LATE (a heavy master filter, a long source job) completes inside it; the block is then delivered late
 * like any over-budget block (the overrun counter sees it, or the late-drop guard when the source graph
 * was cut) and CPU1 stays in service. Every wait below measures it from the same source deadline, so
 * the source join, the stream wait and CPU1's own wait for CPU0's frames agree on one limit.
 * Waiting that long inside the audio interrupt is legal: the foreground watchdog is 1 s or more
 * (hal/fm1_sys.h fm1_wdt_arm) and FM1_CPU1_WORKER_TIMEOUT_TICKS bounds any single wait at 50 ms. */
#define BR_AUDIO_C1_HANG_AFTER_DEADLINE_TICKS BR_AUDIO_HALF_DEADLINE_TICKS
#define BR_AUDIO_C1_HANG_TICKS (2u * BR_AUDIO_HALF_DEADLINE_TICKS)   /* CPU1's own wait for a slice of CPU0 */

/* CPU0's wait for CPU1's streamed master FX (instrument_fx.c br_fx_finish_block).
 * CPU1 has the whole master section left when CPU0 finishes its mix (master FX,
 * the global ladder, which runs from flash, then the limiter over the block), and
 * that grows with the global filter's settings and movement. A fixed 18,415-tick
 * wait took a slow but healthy CPU1 for a hung one and held it in reset for good,
 * and so did "until the source deadline" (a trace showed waits of 19,000 ticks
 * ending exactly there). Wait at least that minimum, and otherwise until the hang
 * limit (source deadline + BR_AUDIO_C1_HANG_AFTER_DEADLINE_TICKS). With no valid deadline
 * (or one that is not this half's) the minimum applies, as before; FM1_CPU1_WORKER_TIMEOUT_TICKS
 * bounds any wait far above this. */
#define BR_AUDIO_STREAM_WAIT_MIN_TICKS 18415u
static inline __attribute__((always_inline)) uint32_t br_audio_stream_wait_ticks(uint32_t deadline_valid,
                                                  uint32_t deadline_tick, uint32_t now)
{
    int32_t left = (int32_t)(deadline_tick + BR_AUDIO_C1_HANG_AFTER_DEADLINE_TICKS - now);
    /* A deadline more than a half away from now is not this half's: keep the minimum. */
    return deadline_valid && left > (int32_t)BR_AUDIO_STREAM_WAIT_MIN_TICKS &&
        left <= (int32_t)BR_AUDIO_C1_HANG_TICKS ? (uint32_t)left : BR_AUDIO_STREAM_WAIT_MIN_TICKS;
}

#endif
