/* SPDX-License-Identifier: GPL-3.0-only */
/* The SMK-37 app's update session (src/ota.c with SMK37_UPDATES) against an in-memory 1 MiB flash.
 * usage: smk_official_ota_host <logical package> <device flash image>
 * Prints the session result, the requests that matter and the record name; never writes files. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smk37_flash_map.h"
#define SMK37_UPDATES 1
#ifndef FELUCCA_ID
#define FELUCCA_ID "SMK-37 Pro_9653121"
#endif
#include "../firmware/src/ota.c"
static uint8_t *pkg, *flash, frame[640]; static uint32_t plen, flen, frame_n, now; static int pending, erasures, notice;
static uint8_t committed[112]; static int commits;
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint8_t d[640], a[530], wire[640], msg[530];
    uint32_t z = ota_unpack7(p + 1, n - 2, d, sizeof d), s = 0;
    if (z < 7 || d[2] != 0x30) return 0;
    uint32_t off = ota_rd32(d + 7), len = ota_rd16(d + 11);
    if (len > 512) return -1;
    memcpy(a, d + 6, 8);
    if (off == 0xE0000002u) { notice++; printf("notice 0xE0000002\n"); return 0; }   /* no answer, as an old host */
    if (off == 0xE0000000u && len == 8) memcpy(a + 8, "success", 8);
    else { if (off > plen || len > plen - off) return -1; memcpy(a + 8, pkg + off, len); }
    for (uint32_t i = 0; i < len + 8; i++) s += a[i];
    msg[0] = 0; msg[1] = 0x59; msg[2] = 0x30; msg[3] = (len + 8) & 255; msg[4] = (len + 8) >> 8; msg[5] = 0;
    memcpy(msg + 6, a, len + 8); msg[14 + len] = (uint8_t)~s;
    frame_n = ota_pack7(msg, len + 15, wire); memcpy(frame, wire, frame_n); pending = 1;
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n) { if (!pending) return 0; *p = frame; *n = frame_n; return 1; }
static void ota_frame_done(void) { pending = 0; }
static uint32_t ota_now_ms(void) { return now++; }
static void ota_idle(void) {}
static int ota_erase(uint32_t off)
{
    if (off < OTA_AREA || off + 4096 > OTA_AREA + OTA_AREA_LEN) { printf("ERASE OUTSIDE STAGING 0x%x\n", off); return -1; }
    memset(flash + off, 0xff, 4096); erasures++; return 0;
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (off < OTA_AREA || off + n > OTA_AREA + OTA_AREA_LEN) { printf("WRITE OUTSIDE STAGING 0x%x\n", off); return -1; }
    memcpy(flash + off, p, n); return 0;
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { if (off + n > flen) return -1; memcpy(p, flash + off, n); return 0; }
static void ota_show(uint32_t s, int32_t code) { printf("stage %u code %d\n", s, code); }
static int ota_commit(const uint8_t *p) { memcpy(committed, p, sizeof committed); commits++; return 0; }
static uint8_t *readfile(const char *name, uint32_t *len)
{
    FILE *f = fopen(name, "rb"); if (!f) exit(2);
    fseek(f, 0, SEEK_END); *len = (uint32_t)ftell(f); rewind(f);
    uint8_t *p = malloc(*len); if (fread(p, 1, *len, f) != *len) exit(2); fclose(f); return p;
}
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    pkg = readfile(argv[1], &plen); flash = readfile(argv[2], &flen);
    uint8_t boot_before[0x4000]; memcpy(boot_before, flash, sizeof boot_before);
    int r = ota_session();
    printf("ota_session=%d erasures=%d commits=%d notice=%d official=%d\n", r, erasures, commits, notice, ota_stage_official);
    if (commits) printf("record name=%s area=0x%x\n", (const char *)committed + 8, ota_rd32(committed + 72));
    printf("boot area untouched=%d\n", !memcmp(boot_before, flash, sizeof boot_before));
    erasures = 0;
    int cleaned = ota_boot_cleanup_official();               /* the next boot of the app */
    uint32_t left = 0;
    for (uint32_t i = OTA_AREA; i < OTA_AREA + OTA_AREA_LEN; ++i) left += flash[i] != 0xffu;
    printf("cleanup=%d window_bytes_left=%u\n", cleaned, left);
    return 0;
}
