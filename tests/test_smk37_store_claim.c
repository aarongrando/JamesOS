/* SPDX-License-Identifier: GPL-3.0-only */
/* SMK-37 store claim (src/platform.c smk37_store_enable) against a simulated 1 MiB flash.
 * run_smk37_units.py extracts the two real functions from platform.c into SMK37_STORE_CLAIM_SRC. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "smk37_flash_map.h"

static uint8_t flash[SMK37_FLASH_SIZE];
static uint8_t ota_flash_ok = 1u;
static unsigned erases, writes, parked;

static int st_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n--) *d++ = flash[smk37_project_physical(off++)];
    return 0;
}
static int flash_worker_park(void) { ++parked; return 0; }
static int flash_worker_resume(void) { assert(parked); --parked; return 0; }
static void fm1_wdt_feed(void) {}
static int fl_erase4k(uint32_t off, uint32_t *took)
{
    *took = 0u;
    assert(parked && !(off & 0xFFFu) && SMK37_APP_WRITE_OK(off, 0x1000u));
    memset(flash + off, 0xff, 0x1000u); ++erases; return 0;
}
static int fl_write(uint32_t off, const uint8_t *src, uint32_t n)
{
    assert(parked && SMK37_APP_WRITE_OK(off, n));
    for (uint32_t i = 0; i < n; ++i) flash[off + i] &= src[i];
    ++writes; return 0;
}

#include SMK37_STORE_CLAIM_SRC

static const uint8_t tag[] = "JamesOS-K store 1";

static void blank(void) { memset(flash, 0xff, sizeof flash); erases = writes = 0u; }
/* The record observed at 0xBA000 after the stock firmware staged the vendor loader. */
static void stock_staging(uint32_t at)
{
    static const uint8_t head[] = {0x99, 0x27, 0xff, 0xff, 0, 0, 0, 0, 0x01, 0x4e, 0, 0, 0x41, 0xff, 0xff, 0xff,
                                   'L', 'O', 'A', 'D', 'E', 'R', '.', 'B', 'I', 'N', 0, 'S', 'H', 'A', '-', '2'};
    memcpy(flash + SMK37_STORE_OWNER, head, sizeof head);
    flash[SMK37_STORE_OWNER + 4] = (uint8_t)at; flash[SMK37_STORE_OWNER + 5] = (uint8_t)(at >> 8);
    flash[SMK37_STORE_OWNER + 6] = (uint8_t)(at >> 16); flash[SMK37_STORE_OWNER + 7] = (uint8_t)(at >> 24);
    for (uint32_t o = SMK37_STORE_OWNER + 64u; o < SMK37_OTA_HI + 0xE00u; ++o) flash[o] = (uint8_t)(o * 37u);
}
static int tagged(void) { return !memcmp(flash + SMK37_STORE_OWNER, tag, sizeof tag); }

int main(void)
{
    /* erased store: claimed, nothing erased */
    blank(); assert(smk37_store_enable() == 1 && tagged() && !erases && writes == 1u);
    /* already claimed: accepted untouched */
    writes = 0u; assert(smk37_store_enable() == 1 && !writes && !erases);
    /* spent stock staging record: only the owner sector is erased, then claimed; staging beyond it is left */
    blank(); stock_staging(SMK37_STORE_OWNER);
    uint8_t ota_before = flash[SMK37_OTA_LO + 100u];
    assert(smk37_store_enable() == 1 && tagged() && erases == 1u);
    for (uint32_t o = SMK37_STORE_OWNER + sizeof tag; o < SMK37_STORE_HI; ++o) assert(flash[o] == 0xffu);
    assert(flash[SMK37_OTA_LO + 100u] == ota_before);
    /* a record naming another address is not the stock staging record */
    blank(); stock_staging(SMK37_STORE_OWNER + 0x1000u); assert(smk37_store_enable() == 0 && !erases && !tagged());
    /* other data in the owner sector stays */
    blank(); flash[SMK37_STORE_OWNER + 3] = 0x12; assert(smk37_store_enable() == 0 && !erases);
    /* staging record but data in the settings sector or a project sector: nothing is erased */
    blank(); stock_staging(SMK37_STORE_OWNER); flash[SMK37_SETTINGS_LO + 9] = 0; assert(smk37_store_enable() == 0 && !erases);
    blank(); stock_staging(SMK37_STORE_OWNER); flash[SMK37_PROJECT_LO + 0x2000u] = 0; assert(smk37_store_enable() == 0 && !erases);
    /* the two occupied physical sectors are outside the logical project area */
    blank(); flash[0xAB000u] = 0; flash[0xAF000u] = 0; assert(smk37_store_enable() == 1 && tagged());
    /* unknown flash chip: never claimed */
    blank(); ota_flash_ok = 0u; assert(smk37_store_enable() == 0 && !writes); ota_flash_ok = 1u;
    puts("SMK-37 store claim: ok");
    return 0;
}
