/* The master low-pass block pre-pass (br_master_ladder_block, hooked at the start of br_master_fx_process_range) must
 * leave the master section bit-for-bit where the per-frame path leaves it. The same long random stereo scenario runs
 * twice through the real master code, once with the pre-pass switched off (every frame through br_master_ladder_frame)
 * and once on; the output of every block and the whole master state after every block (filter states, tail, wow,
 * heat, resets) are hashed and must match. The scenario moves the filter, resonance and heat settings, uses blocks
 * of 256 and odd sizes, slices of 16 frames, odd cuts and whole blocks (the stream worker and the serial path), long
 * silences that cross the FS/4 fade and the tail zeroing, loud and clipped input, and leaves and re-enters the
 * low-pass side. It also checks that the pre-pass really ran. */
#define DINGHY_HOST 1
#define BR_MASTER_LADDER_BLOCK_TEST_SWITCH 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include <assert.h>
#include <stddef.h>

#define MAXN 256u
static uint32_t g_rng;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint32_t rnd_n(uint32_t n) { return rnd() % n; }
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

#define MAXBLOCKS 60000u
typedef struct { uint64_t out[MAXBLOCKS], state[MAXBLOCKS]; uint32_t blocks, hits, mono_hits, unstable_hits; uint64_t frames; } run_t;
static run_t run_off, run_on;
static uint32_t g_slice_rng = 7u; /* slicing choices come from their own stream so the modes see the same signal */
static uint32_t srnd_n(uint32_t n) { g_slice_rng ^= g_slice_rng << 13; g_slice_rng ^= g_slice_rng >> 17; g_slice_rng ^= g_slice_rng << 5; return g_slice_rng % n; }
static uint32_t g_split_blocks, g_split_slices;
static uint32_t g_capture_block = 0xFFFFFFFFu; static unsigned char g_capture[2048]; static uint32_t g_capture_len;

/* mode 0: pre-pass off; 1: pre-pass on; 2: as 1, and blocks the split would take run in the split order slice by slice;
 * 3: as 2, but all of C1's scan and channel 0 first, then all of C0's channel 1, then the post stage; 6: random interleaving of C1's scan/channel 0/post and C0's channel 1 under the real ordering constraints; 4: as 1 and 5: pre-pass off, both with the
 * stream job's 16-frame slices only (the baseline for 2 and 3: the skipped-silence rule makes wow and bit-hold phase depend on
 * the slicing, so only equal slicings are comparable). */
static void scenario(int mode, run_t *r, uint32_t seed, uint32_t segments)
{
    int32_t st[MAXN * 2u];
    uint32_t filter = 30, res = 0, heat_type = BR_HEAT_BIT, heat_amount = 0, kind = 0, amp = 0;
    uint32_t seg, glitch_seed = 1u, diverge_block = 0u;
    g_rng = seed;
    br_app_init(); fm1_ms = 1000u;
    memset(&br_gf, 0, sizeof br_gf);
    br_master.fx.env_depth = 0;
    br_master.revision = 0u;
    br_master.fx.filter = filter; br_master.fx.resonance = res;
    br_master.fx.heat_type = heat_type; br_master.fx.heat_amount = heat_amount;
    br_master_fx_init();
    br_master_ladder_block_enable = (uint32_t)(mode != 0 && mode != 5);
    g_slice_rng = seed | 1u;
    br_master_ladder_block_hits = 0u; br_master_ladder_block_mono_hits = 0u; br_master_ladder_block_unstable_hits = 0u;
    r->blocks = 0u; r->frames = 0u;
    for (seg = 0; seg < segments && r->blocks < MAXBLOCKS - 700u; ++seg) {
        static const uint32_t amps[] = { 1, 3, 100, 2000, 12000, 32767, 100000, 600000, 4000000 };
        uint32_t nblocks = 3u + rnd_n(rnd_n(3) ? 120u : 700u);
        uint32_t odd = rnd_n(4) == 0, pattern = rnd_n(4);
        if (rnd_n(3) == 0) { /* low-pass side mostly, sometimes bypass, sometimes the high-pass */
            uint32_t k = rnd_n(10);
            filter = k < 7 ? rnd_n(63u) : k < 8 ? 63u + rnd_n(2u) : 65u + rnd_n(63u);
        }
        if (rnd_n(3) == 0) res = rnd_n(3) == 0 ? 0u : rnd_n(128u);
        if (rnd_n(4) == 0) {
            uint32_t k = rnd_n(3);
            heat_type = k == 0 ? BR_HEAT_BIT : k == 1 ? BR_HEAT_SAT : BR_HEAT_WOW;
            heat_amount = heat_type == BR_HEAT_BIT && rnd_n(2) ? 0u : rnd_n(128u);
        }
        kind = rnd_n(9); /* 0 noise, 1 silence, 2 sine, 3 bursts, 4 sparse clicks, 5 mono noise, 6 mono sine,
                            7 mono, then one channel off by one frame, 8 mono with rare one-LSB splits */
        if (rnd_n(5) == 0) { /* equal channel states again: a fresh master */
            br_master.revision++;
            br_master.fx.filter = filter; br_master.fx.resonance = res;
            br_master.fx.heat_type = heat_type; br_master.fx.heat_amount = heat_amount;
            br_master.revision++;
            memset(&br_gf, 0, sizeof br_gf);
            br_master_fx_init();
        }
        diverge_block = rnd_n(nblocks + 1u);
        if (kind == 1 && rnd_n(2)) nblocks = 380u + rnd_n(500u); /* crosses FS/4 and the 2 s tail */
        if (kind == 1 && nblocks > 700u) nblocks = 700u;
        amp = amps[rnd_n(sizeof amps / sizeof amps[0])];
        br_master.revision++;
        br_master.fx.filter = filter; br_master.fx.resonance = res;
        br_master.fx.heat_type = heat_type; br_master.fx.heat_amount = heat_amount;
        br_master.revision++;
        for (uint32_t b = 0; b < nblocks && r->blocks < MAXBLOCKS; ++b) {
            uint32_t n = odd && rnd_n(2) ? 1u + rnd_n(MAXN) : (rnd_n(8) == 0 ? 1u + rnd_n(MAXN) : 256u);
            uint32_t active, first = 0u, slice;
            if (n > MAXN) n = MAXN;
            for (uint32_t i = 0; i < n; ++i) {
                int32_t l = 0, rr = 0;
                glitch_seed = glitch_seed * 1664525u + 1013904223u;
                switch (kind) {
                case 0: l = (int32_t)(((int64_t)(int32_t)glitch_seed * amp) >> 31);
                        glitch_seed = glitch_seed * 1664525u + 1013904223u;
                        rr = (int32_t)(((int64_t)(int32_t)glitch_seed * amp) >> 31); break;
                case 2: l = (int32_t)(sin((double)(r->frames + i) * 0.03) * (double)amp);
                        rr = (int32_t)(sin((double)(r->frames + i) * 0.047) * (double)amp); break;
                case 3: if ((b & 7u) < 3u) { l = (int32_t)(glitch_seed >> 8) % (int32_t)(amp + 1); rr = -l / 2; } break;
                case 4: if ((glitch_seed >> 24) == 0) { l = (int32_t)amp; rr = -(int32_t)amp; } break;
                case 5: l = (int32_t)(((int64_t)(int32_t)glitch_seed * amp) >> 31); rr = l; break;
                case 6: l = (int32_t)(sin((double)(r->frames + i) * 0.031) * (double)amp); rr = l; break;
                case 7: l = (int32_t)(((int64_t)(int32_t)glitch_seed * amp) >> 31); rr = l;
                        if (b >= diverge_block && i == (b * 7u) % 16u) rr = l + 1; /* a frame that differs, then stereo-ish */
                        break;
                case 8: l = (int32_t)(sin((double)(r->frames + i) * 0.02) * (double)amp); rr = l;
                        if ((glitch_seed >> 20) == 0u) rr = l ^ 1; /* rare one-LSB split */
                        break;
                default: break;
                }
                st[2u * i] = l; st[2u * i + 1u] = rr;
            }
            {   /* the shared reverb's return for this block: present or absent, random */
                uint32_t gr_on = rnd_n(3) != 0;
                static uint32_t blk_seq = 1000u;
                int32_t *wl = br_gr_send[br_gr_cur ^ 1u][BR_GR_WET_L_ROW], *wr = br_gr_send[br_gr_cur ^ 1u][BR_GR_WET_R_ROW];
                uint32_t wamp = 1u + rnd_n(60000u), samp = rnd_n(4) == 0 ? 8u : 0u;
                ++blk_seq; br_fx_block_seq = blk_seq;
                for (uint32_t i = 0; i < n; ++i) {
                    glitch_seed = glitch_seed * 1664525u + 1013904223u;
                    wl[i] = gr_on ? (int32_t)(((int64_t)(int32_t)glitch_seed * (int64_t)wamp) >> (31 - samp)) : 0;
                    glitch_seed = glitch_seed * 1664525u + 1013904223u;
                    wr[i] = gr_on ? (int32_t)(((int64_t)(int32_t)glitch_seed * (int64_t)wamp) >> (31 - samp)) : 0;
                }
                br_gr_run = gr_on; br_gr_out_seq = gr_on ? blk_seq : blk_seq - 1u;
            }
            active = br_master_fx_begin_block(n);
            if (active) {
                br_master_ctl_t kc;
                uint32_t split = (mode == 2 || mode == 3 || mode == 6) && br_master_split_decide(n, &kc);
                if (split) {
                    uint32_t fits[MAXN / 16u + 1u], k, nsl = (n + 15u) / 16u;
                    ++g_split_blocks; g_split_slices += nsl;
                    if (mode == 2) {
                        for (k = 0; k < nsl; ++k) {
                            uint32_t b0 = k * 16u, e0 = b0 + 16u > n ? n : b0 + 16u, fit = br_master_split_scan(st, b0, e0);
                            br_master_split_filter0(st, b0, e0, fit, &kc);
                            br_master_split_channel1(st, b0, e0, &kc);
                            br_master_state.ladder_pre = 2u;
                            br_master_fx_process_range(st, b0, e0);
                            assert(br_master_state.ladder_pre == 0u);
                        }
                    } else if (mode == 6) {
                        /* C1 runs S_k F_k for each slice as C0 produces it, and posts opportunistically; C0 runs C_k in order
                         * after S_k. Random interleaving that respects only: S in order, F_k right after S_k on C1, C_k after
                         * S_k, post_k in order after F_k and C_k. */
                        uint32_t s_done = 0, f_done = 0, c_done = 0, p_done = 0;
                        while (p_done < nsl) {
                            uint32_t opt[3], no = 0, pick;
                            uint32_t c1_next = f_done < nsl ? (s_done == f_done ? 1u : 2u) : 0u; /* 1: S, 2: F, 0: only posts */
                            int post_ok = p_done < f_done && p_done < c_done;
                            if (c1_next) opt[no++] = c1_next;
                            if (post_ok) opt[no++] = 3u;
                            if (c_done < s_done) opt[no++] = 4u;
                            assert(no);
                            pick = opt[srnd_n(no)];
                            if (pick == 1u && post_ok && srnd_n(2)) pick = 3u;
                            { uint32_t kk = pick == 1u ? s_done : pick == 2u ? f_done : pick == 3u ? p_done : c_done;
                              uint32_t b0 = kk * 16u, e0 = b0 + 16u > n ? n : b0 + 16u;
                              if (pick == 1u) { fits[kk] = br_master_split_scan(st, b0, e0); ++s_done; }
                              else if (pick == 2u) { br_master_split_filter0(st, b0, e0, fits[kk], &kc); ++f_done; }
                              else if (pick == 4u) { br_master_split_channel1(st, b0, e0, &kc); ++c_done; }
                              else { br_master_state.ladder_pre = 2u; br_master_fx_process_range(st, b0, e0); assert(br_master_state.ladder_pre == 0u); ++p_done; } }
                        }
                    } else {
                        for (k = 0; k < nsl; ++k) { uint32_t b0 = k * 16u, e0 = b0 + 16u > n ? n : b0 + 16u; fits[k] = br_master_split_scan(st, b0, e0); br_master_split_filter0(st, b0, e0, fits[k], &kc); }
                        for (k = 0; k < nsl; ++k) { uint32_t b0 = k * 16u, e0 = b0 + 16u > n ? n : b0 + 16u; br_master_split_channel1(st, b0, e0, &kc); }
                        for (k = 0; k < nsl; ++k) { uint32_t b0 = k * 16u, e0 = b0 + 16u > n ? n : b0 + 16u; br_master_state.ladder_pre = 2u; br_master_fx_process_range(st, b0, e0); assert(br_master_state.ladder_pre == 0u); }
                    }
                } else {
                    switch (pattern) {
                    case 0: slice = 16u; break;
                    case 1: slice = n; break;
                    case 2: slice = 1u + srnd_n(40u); break;
                    default: slice = srnd_n(2) ? 16u : n; break;
                    }
                    if (mode >= 2) slice = 16u; /* the stream job's slices */
                    while (first < n) {
                        uint32_t end = first + slice > n ? n : first + slice;
                        if (br_gr_run && br_gr_out_seq == br_fx_block_seq) br_gr_add_range(st, first, end);
                        br_master_fx_process_range(st, first, end);
                        assert(br_master_state.ladder_pre == 0u);
                        first = end;
                        if (pattern == 3 && srnd_n(3) == 0 && mode < 2) slice = 1u + srnd_n(33u);
                    }
                }
            }
            r->out[r->blocks] = fnv(1469598103934665603ull, st, n * 8u);
            r->state[r->blocks] = fnv(1469598103934665603ull, &br_master_state, sizeof br_master_state);
            if (r->blocks == g_capture_block) { g_capture_len = sizeof br_master_state; memcpy(g_capture, &br_master_state, sizeof br_master_state); }
            r->frames += n;
            ++r->blocks;
        }
    }
    r->hits = br_master_ladder_block_hits; r->mono_hits = br_master_ladder_block_mono_hits; r->unstable_hits = br_master_ladder_block_unstable_hits;
}

int main(int argc, char **argv)
{
    uint32_t segments = argc > 1 ? (uint32_t)atoi(argv[1]) : 120u;
    uint64_t frames = 0; uint32_t hits = 0, ranges_total = 0, mono = 0, unstable = 0;
    for (uint32_t seed = 1u; seed <= 3u; ++seed) {
        scenario(0, &run_off, 0x9E3779B1u * seed, segments);
        scenario(1, &run_on, 0x9E3779B1u * seed, segments);
        assert(run_off.blocks == run_on.blocks && run_off.hits == 0u);
        for (uint32_t b = 0; b < run_on.blocks; ++b) {
            if (run_off.out[b] != run_on.out[b]) { fprintf(stderr, "seed %u block %u: output differs\n", seed, b); return 1; }
            if (run_off.state[b] != run_on.state[b]) { fprintf(stderr, "seed %u block %u: master state differs\n", seed, b); return 1; }
        }
        static run_t run_s16;
        scenario(5, &run_s16, 0x9E3779B1u * seed, segments);
        { static run_t run_pp16; scenario(4, &run_pp16, 0x9E3779B1u * seed, segments); for (uint32_t b = 0; b < run_pp16.blocks; ++b) if (run_pp16.out[b] != run_s16.out[b] || run_pp16.state[b] != run_s16.state[b]) { fprintf(stderr, "seed %u block %u: pre-pass differs at 16-frame slices\n", seed, b); return 1; } }
        for (int mode = 2; mode <= 6; ++mode) {
            if (mode == 4 || mode == 5) continue;
            static run_t run_split;
            scenario(mode, &run_split, 0x9E3779B1u * seed, segments);
            assert(run_split.blocks == run_s16.blocks);
            for (uint32_t b = 0; b < run_split.blocks; ++b) {
                if (run_s16.out[b] != run_split.out[b]) { fprintf(stderr, "seed %u mode %d block %u: split output differs\n", seed, mode, b); return 1; }
                if (run_s16.state[b] != run_split.state[b]) {
                    static unsigned char ref_state[2048]; uint32_t o;
                    fprintf(stderr, "seed %u mode %d block %u: split master state differs\n", seed, mode, b);
                    g_capture_block = b; scenario(5, &run_s16, 0x9E3779B1u * seed, segments);
        { static run_t run_pp16; scenario(4, &run_pp16, 0x9E3779B1u * seed, segments); for (uint32_t b = 0; b < run_pp16.blocks; ++b) if (run_pp16.out[b] != run_s16.out[b] || run_pp16.state[b] != run_s16.state[b]) { fprintf(stderr, "seed %u block %u: pre-pass differs at 16-frame slices\n", seed, b); return 1; } } memcpy(ref_state, g_capture, g_capture_len);
                    scenario(mode, &run_split, 0x9E3779B1u * seed, segments);
                    for (o = 0; o < g_capture_len; ++o) if (ref_state[o] != g_capture[o]) fprintf(stderr, "  byte %u: %02x vs %02x\n", o, ref_state[o], g_capture[o]);
                    fprintf(stderr, "  ladder at byte %u (state size %u)\n", (unsigned)offsetof(br_master_state_t, ladder), g_capture_len);
                    return 1; }
            }
        }
        frames += run_on.frames; hits += run_on.hits; mono += run_on.mono_hits; unstable += run_on.unstable_hits; ranges_total += run_on.blocks;
    }
    assert(hits > 1000u); /* the pre-pass really ran */
    assert(unstable > 500u); /* and ranges of blocks whose controls were moving */
    assert(mono > 500u && mono < hits); /* and both the mono and the stereo forms of it */
    assert(g_split_blocks > 200u); /* and the split order ran */
    printf("master ladder block: %llu frames in %u blocks, output and master state identical to the per-frame path after every block; pre-pass engaged on %u ranges, %u of them on the mono path, %u in blocks with moving controls; the split core orders (slice by slice, phased, and random interleavings with C1 running ahead) matched on %u blocks (%u slices) with and without the reverb return\n",
           (unsigned long long)frames, ranges_total, hits, mono, unstable, g_split_blocks, g_split_slices);
    return 0;
}
