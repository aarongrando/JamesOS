/* SPDX-License-Identifier: GPL-3.0-only */
/* Read-only clock and power register probe (diagnostic 0x9C). It reads the five clock registers the FM-1's
 * 396 MHz supply policy checks (fm1_vendor360_target.h) and the eleven P33 registers of that policy, plus
 * the reset reasons found at boot. It writes nothing. Used to compare an SMK-37 family keyboard with the FM-1. */
#pragma once
#include <stdint.h>
#include "fm1_vendor360_policy.h"
static void fm1_clock_probe_words(uint32_t out[19], uint32_t p3_rst, uint32_t rst_src)
{
    uint32_t i;
    out[0] = 1u;                                           /* schema */
    out[1] = *(volatile uint32_t *)(uintptr_t)0x10008u;    /* clock divider */
    out[2] = *(volatile uint32_t *)(uintptr_t)0x1000cu;    /* clock control 0 */
    out[3] = *(volatile uint32_t *)(uintptr_t)0x10014u;    /* clock source */
    out[4] = *(volatile uint32_t *)(uintptr_t)0x119a8u;    /* PLL 0 */
    out[5] = *(volatile uint32_t *)(uintptr_t)0x119acu;    /* PLL 1 */
    out[6] = p3_rst; out[7] = rst_src;
    for (i = 0u; i < FM1_VP_COUNT; ++i) out[8u + i] = fm1_p33_read(fm1_vp_reg(i));
}
