/* SPDX-License-Identifier: GPL-3.0-only */
/* The master filter cue and the filter chart while the filter moves.
 *  1. SELECT turning the master filter posts no message at a detent. One "MASTER FILTER <value>" cue appears
 *     BR_FILTER_CUE_MS (0.8 s) after the last change, with the final value; a new change restarts the wait;
 *     a turn that changes nothing (the end of the range) posts nothing; the snap shortcut (GLO + knob 1 snap
 *     on GLOBAL / FILTER, then SELECT) is cued the same way.
 *  2. On GLOBAL / FILTER and on the track filter page, a sweep of knob 1 or of SELECT redraws the chart at most
 *     every BR_CHART_MS, sends few bytes per detent, and once it has settled the sliced screen equals a fresh
 *     full redraw (also after the cue has come and gone).
 * The real app loop runs once per host millisecond; input goes in as the host injects it (tests/test_draw_load.c). */
#define DINGHY_HOST 1
#define UI_REFRESH_TEST_COUNTERS 1
static unsigned filter_cue_clock;               /* a made-up clock: a pass ends after a few regions, as on the device */
#define BR_DRAW_NOW() ((uint32_t)(filter_cue_clock += 16800u))
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include "hd_look.h"
#include "host_intro.h"

static uint64_t sent;                           /* pixel bytes sent to the LCD since it was last cleared */
static uint16_t shot[240 * 240];
static uint32_t msg_posts;                      /* how often a message was posted (its start time changed) */
static uint32_t last_start;

static void tick(void)
{
    fm1_ms++;
    ui_test_lcd_pixel_bytes = 0;
    br_app_tick();
    sent += ui_test_lcd_pixel_bytes;
    if (br_ui.message[0] && br_ui.message_start != last_start) { ++msg_posts; last_start = br_ui.message_start; }
}
static void run_ms(uint32_t n) { while (n--) tick(); }
static void quiet(void)                         /* nothing pending, no message on show */
{
    run_ms(3000);
    br_ui.message[0] = 0; br_ui.message_until = 0u; br_ui.redraw = 1u;
    run_ms(200);
    msg_posts = 0; last_start = br_ui.message_start;
}
static void scene(uint32_t page, uint32_t screen, uint32_t part)
{
    br_ui.part = part; br_ui.page = page; br_ui.screen[page] = screen; br_ui.redraw = 1u;
    br_master.fx.filter = 40u; br_part[part].sound.filter = 40u;
    br_ui.filter_cue = 0u;
    quiet();
}
static void expect_text(char *out, uint32_t value)
{
    char text[24];
    br_filter_text(text, (int32_t)value);
    br_text_copy(out, "MASTER FILTER ", 25u);
    br_text_append(out, text, 25u);
}

/* Compare the screen as the sliced frames left it with a fresh full redraw of the same state. */
static void same_as_full(const char *what)
{
    uint32_t n = 0;
    while ((br_draw_more || br_ui.redraw) && n < 400u) { ui_test_lcd_pixel_bytes = 0; br_app_tick(); ++n; }
    memcpy(shot, fb, sizeof shot);
    br_draw_layout_old = 0xffffffffu; br_ui.redraw = 1u; br_draw();
    uint32_t differ = 0;
    for (uint32_t i = 0; i < 240u * 240u; ++i) differ += shot[i] != fb[i];
    HD_CHECK(!differ, "%s: the settled screen differs from a full redraw in %u pixels", what, differ);
}

#define SWEEP_DETENTS 12u
#define SWEEP_GAP 40u
/* Bytes per detent (the sweep and 0.4 s after it) must stay under this: before the chart was rate limited it was
 * about 14,700 on these pages; now about 6,400 + 900. */
#define CHART_BUDGET 8500u
#define HOME_BUDGET 200u
static void sweep(uint32_t encoder, uint32_t *chart_draws, uint64_t *bytes, uint32_t *posts)
{
    uint32_t seen = br_chart_shown.at, draws = 0;
    msg_posts = 0; sent = 0;
    for (uint32_t i = 0; i < SWEEP_DETENTS; ++i) {
        host_encoder[encoder] += (i & 1u) ? 1 : 2;
        for (uint32_t m = 0; m < SWEEP_GAP; ++m) {
            tick();
            if (br_chart_shown.at != seen) { seen = br_chart_shown.at; ++draws; }
        }
    }
    *posts = msg_posts;
    run_ms(400);
    *bytes = sent; *chart_draws = draws;
}

int main(int argc, char **argv)
{
    char want[25];
    uint32_t t0;
    (void)argc; (void)argv;
    host_init_resources();
    br_app_init(); host_after_intro(); br_intro_until = 0; fm1_ms = 5000u; br_ui.redraw = 1u;
    run_ms(500);

    /* ---- the cue on HOME ---- */
    scene(BR_PAGE_HOME, 0, 0);
    {
        uint32_t posts, draws; uint64_t bytes;
        sweep(0, &draws, &bytes, &posts);
        HD_CHECK(!posts, "HOME: %u messages during a SELECT sweep (want none)", posts);
        HD_CHECK(bytes <= HOME_BUDGET * SWEEP_DETENTS, "HOME: %llu bytes sent by a SELECT sweep (want almost none)", (unsigned long long)bytes);
    }
    /* the last detent was 0.4 s ago; the cue is due 0.8 s after it */
    HD_CHECK(br_ui.filter_cue, "HOME: the cue is pending after the sweep");
    {
        uint32_t final_value = br_master.fx.filter;
        HD_CHECK(br_ui.filter_cue_at == br_ui.edit_time + BR_FILTER_CUE_MS, "HOME: the cue is due 0.8 s after the last change");
        HD_CHECK(BR_FILTER_CUE_MS == 800u, "the cue wait is 0.8 s");
        run_ms(br_ui.filter_cue_at - 1u - fm1_ms);   /* up to 1 ms before it is due */
        HD_CHECK(!br_ui.message[0] && !msg_posts, "HOME: no cue before 0.8 s");
        run_ms(2);
        HD_CHECK(msg_posts == 1u, "HOME: the cue appeared once (%u)", msg_posts);
        expect_text(want, final_value);
        HD_CHECK(!strcmp(br_ui.message, want), "HOME: the cue shows the final value: '%s', want '%s'", br_ui.message, want);
        run_ms(3000);
        HD_CHECK(msg_posts == 1u, "HOME: no second cue (%u)", msg_posts);
    }

    /* a second sweep restarts the wait: a detent at t0, another 500 ms later, the cue 800 ms after that one */
    scene(BR_PAGE_HOME, 0, 0);
    t0 = fm1_ms;
    host_encoder[0] += 1; run_ms(500);
    host_encoder[0] += 1;
    {
        run_ms(1);
        HD_CHECK(br_ui.filter_cue_at == br_ui.edit_time + BR_FILTER_CUE_MS && br_ui.edit_time - t0 >= 500u,
                 "restart: the second detent (%u ms after the first) moved the due time", br_ui.edit_time - t0);
        run_ms(br_ui.filter_cue_at - 1u - fm1_ms);   /* past 0.8 s after the first detent, 1 ms short of the second's */
        HD_CHECK(fm1_ms - t0 > BR_FILTER_CUE_MS && !msg_posts && !br_ui.message[0],
                 "restart: no cue 0.8 s after the first detent, the second restarted the wait");
        run_ms(2);
        HD_CHECK(msg_posts == 1u, "restart: the cue came 0.8 s after the second detent (%u)", msg_posts);
        expect_text(want, br_master.fx.filter);
        HD_CHECK(!strcmp(br_ui.message, want), "restart: final value '%s', want '%s'", br_ui.message, want);
    }

    /* nothing changes at the end of the range, so nothing fires */
    scene(BR_PAGE_HOME, 0, 0);
    br_master.fx.filter = 127u; run_ms(100);
    host_encoder[0] += 3; run_ms(2000);
    HD_CHECK(!msg_posts && !br_ui.filter_cue && br_master.fx.filter == 127u, "top of the range: no cue (%u)", msg_posts);
    br_master.fx.filter = 0u; run_ms(100);
    host_encoder[0] -= 3; run_ms(2000);
    HD_CHECK(!msg_posts && !br_ui.filter_cue && br_master.fx.filter == 0u, "bottom of the range: no cue (%u)", msg_posts);
    scene(BR_PAGE_HOME, 0, 0);
    run_ms(2000);
    HD_CHECK(!msg_posts, "idle: no cue");

    /* undo bookkeeping is as before: one entry per sweep, the edit key and time are set */
    scene(BR_PAGE_HOME, 0, 0);
    host_encoder[0] += 1; run_ms(5);
    HD_CHECK(br_ui.edit_key == 4096u + br_ui.part && fm1_ms - br_ui.edit_time < 10u, "edit key and time are kept");

    /* ---- the snap shortcut (GLO held on GLOBAL / FILTER, SELECT jumps to the next landmark) ---- */
    scene(BR_PAGE_MIX, 1, 0);
    br_ui.glo_pending = 1u; br_ui.glo_snap = 1u;
    {
        uint32_t gen = br_master.fx.filter_snap_generation;
        host_encoder[0] += 1; run_ms(5);
        HD_CHECK(br_master.fx.filter == 64u, "snap: SELECT moved the filter to %u (want 64)", br_master.fx.filter);
        HD_CHECK(br_master.fx.filter_snap_generation == gen + 1u, "snap: the generation advanced");
        HD_CHECK(!msg_posts, "snap: no message at the turn");
        run_ms(800);
        expect_text(want, 64u);
        HD_CHECK(msg_posts == 1u && !strcmp(br_ui.message, want), "snap: one cue '%s', want '%s' (%u posts)", br_ui.message, want, msg_posts);
    }
    br_ui.glo_pending = 0u; br_ui.glo_snap = 0u; br_ui.glo_used = 0u;

    /* ---- the chart: GLOBAL / FILTER, knob 1 and SELECT; the track filter page, knob 1 ---- */
    static const struct { const char *name; uint32_t page, screen, encoder; } pages[] = {
        {"GLOBAL / FILTER, knob 1", BR_PAGE_MIX, 1u, 2u},
        {"GLOBAL / FILTER, SELECT", BR_PAGE_MIX, 1u, 0u},
        {"track filter, knob 1", BR_PAGE_HOME, 1u, 2u},
    };
    for (uint32_t i = 0; i < sizeof pages / sizeof pages[0]; ++i) {
        uint32_t posts, draws; uint64_t bytes;
        scene(pages[i].page, pages[i].screen, 0);
        sweep(pages[i].encoder, &draws, &bytes, &posts);
        HD_CHECK(!posts, "%s: %u messages during the sweep", pages[i].name, posts);
        HD_CHECK(draws >= 2u, "%s: the chart was redrawn %u times (it should follow the knob)", pages[i].name, draws);
        HD_CHECK(draws <= 1u + SWEEP_DETENTS * SWEEP_GAP / BR_CHART_MS,
                 "%s: the chart was redrawn %u times in %u ms (at most one per %u ms)", pages[i].name, draws,
                 SWEEP_DETENTS * SWEEP_GAP, BR_CHART_MS);
        HD_CHECK(bytes <= (uint64_t)CHART_BUDGET * SWEEP_DETENTS, "%s: %llu bytes per detent (budget %u)", pages[i].name,
                 (unsigned long long)(bytes / SWEEP_DETENTS), CHART_BUDGET);
        printf("  %-28s %5llu B per detent, %u chart redraws in %u ms\n", pages[i].name,
               (unsigned long long)(bytes / SWEEP_DETENTS), draws, SWEEP_DETENTS * SWEEP_GAP);
        same_as_full(pages[i].name);             /* settled, before the cue */
        run_ms(4000);                            /* the cue came and went */
        same_as_full(pages[i].name);
        /* the chart shows the final values: moving the knob once more and waiting draws it once more */
        host_encoder[pages[i].encoder] += 1; run_ms(1);
        run_ms(BR_CHART_MS + 2u * BR_FRAME_MS);
        same_as_full(pages[i].name);
    }
    /* a track change on the track filter page shows that track's values at once */
    scene(BR_PAGE_HOME, 1, 0);
    br_part[1].sound.filter = 100u; br_part[1].sound.filter_resonance = 90u;
    br_part[0].sound.filter = 20u;
    host_encoder[2] += 1; run_ms(60);
    br_ui.part = 1u; br_ui.redraw = 1u; run_ms(60);
    same_as_full("track filter, other track");
    return hd_finish("Master filter cue and filter chart");
}
