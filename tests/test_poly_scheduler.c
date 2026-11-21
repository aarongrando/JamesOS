/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the real source queue with six isolated experimental voices. */
#define BR_POLY_SCHEDULER_EXPERIMENT 1
#define main reconstruction_fixture_main
#include "test_partial_frame_reconstruction.c"
#undef main
#include "../firmware/plaits/upstream/plaits/wavetable_pack.h"
#if defined(BR_POLY_TEST_TWO_CORES)
#include <pthread.h>
static void *poly_worker(void *unused)
{
    (void)unused;
    test_core_id = 1u;
    return (void *)(uintptr_t)br_audio_source_queue_run(1u);
}
#endif

/* Chord loudness through the polyphonic post stage: RMS and peak of the track row
 * with `count` notes held, after the mix gain has settled. */
static void chord_level(uint32_t part, uint32_t model, uint32_t count, uint32_t filter, uint32_t chorus,
                        double *rms, uint32_t *peak)
{
    br_sound_t sound;
    double acc = 0.0; uint32_t samples = 0u, top = 0u;
    configure(BR_KIT_808, 0u);
    br_preset_sound(BR_FIRST_PLAITS + model, &sound);
    sound.attack = 0u; sound.decay = 0u; sound.sustain = 127u; sound.level = 127u;
    sound.delay = 0u; sound.reverb = 0u; sound.drive = 0u;
    sound.filter = filter; sound.filter_resonance = 0u; sound.filter_keytrack = 0u;
    br_part[part].sound = sound; br_part[part].fx.mod_mix = chorus; br_part[part].revision += 2u;
    br_snapshot(part); br_build_effective(part);
    assert(br_poly_scheduler_enter(part, 1));
    static const uint8_t notes[6] = {48u, 55u, 60u, 64u, 67u, 71u};
    for (uint32_t i = 0; i < count; ++i) br_engine_note_on_tagged(part, notes[i], 127u, i + 1u);
    for (uint32_t block = 0; block < 80u; ++block) {
        br_engine_render(bus, BR_BLOCK);
        assert(br_fx_fusion_complete(bus, BR_BLOCK));
        br_fx_fusion_cancel();
        if (block >= 40u) for (uint32_t i = 0; i < BR_BLOCK; ++i) {
            int32_t v = bus[part][i]; uint32_t a = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
            acc += (double)v * v; ++samples; if (a > top) top = a;
        }
    }
    for (uint32_t i = 0; i < count; ++i) br_engine_note_off_tagged(part, notes[i], i + 1u);
    br_engine_render(bus, BR_BLOCK); br_fx_fusion_cancel();
    assert(br_poly_scheduler_leave(1));
    *rms = sqrt(acc / samples); *peak = top;
}

/* The whole chain to the master output with delay, drive and reverb on: peak, the longest run of samples
 * stuck at that peak (a hard clip shows as a flat top) and the largest second difference (a click). */
static void chord_master(uint32_t part, uint32_t count, uint32_t level, uint32_t fx, uint32_t *peak, uint32_t *flat, uint32_t *spike, double *rms)
{
    br_sound_t sound;
    static int32_t out[2u * BR_BLOCK];
    static int32_t keep[40u * BR_BLOCK];
    uint32_t kept = 0u;
    configure(BR_KIT_808, 0u);
    br_preset_sound(BR_FIRST_PLAITS + 8u, &sound);
    sound.attack = 0u; sound.decay = 0u; sound.sustain = 127u; sound.level = level;
    sound.delay = fx ? 100u : 0u; sound.reverb = fx ? 64u : 0u; sound.drive = fx ? 64u : 0u;
    sound.filter = 64u; sound.filter_resonance = 0u; sound.filter_keytrack = 0u;
    br_part[part].sound = sound; br_part[part].fx.mod_mix = 0u; br_part[part].fx.delay_feedback = 90u;
    br_master.comp.mix = 0u; br_master.revision += 2u;   /* the fixture's compressor would level everything */
    br_part[part].revision += 2u;
    br_snapshot(part); br_build_effective(part);
    assert(br_poly_scheduler_enter(part, 1));
    static const uint8_t notes[6] = {48u, 55u, 60u, 64u, 67u, 71u};
    for (uint32_t i = 0; i < count; ++i) br_engine_note_on_tagged(part, notes[i], 127u, i + 1u);
    for (uint32_t block = 0; block < 80u; ++block) {
        br_engine_render(bus, BR_BLOCK);
        assert(br_fx_fusion_complete(bus, BR_BLOCK));
        br_fx_process(bus, out, BR_BLOCK);
        if (block >= 40u) for (uint32_t i = 0; i < BR_BLOCK; ++i) keep[kept++] = out[2u * i];
    }
    for (uint32_t i = 0; i < count; ++i) br_engine_note_off_tagged(part, notes[i], i + 1u);
    br_engine_render(bus, BR_BLOCK); br_fx_fusion_cancel();
    assert(br_poly_scheduler_leave(1));
    uint32_t top = 0u, run = 0u, longest = 0u, jump = 0u; double acc = 0.0;
    for (uint32_t i = 0; i < kept; ++i) { uint32_t a = keep[i] < 0 ? 0u - (uint32_t)keep[i] : (uint32_t)keep[i]; if (a > top) top = a; acc += (double)keep[i] * keep[i]; }
    for (uint32_t i = 0; i < kept; ++i) {
        uint32_t a = keep[i] < 0 ? 0u - (uint32_t)keep[i] : (uint32_t)keep[i];
        run = a == top ? run + 1u : 0u; if (run > longest) longest = run;
        if (i >= 2u) { int64_t d = (int64_t)keep[i] - 2 * (int64_t)keep[i - 1] + keep[i - 2]; uint32_t m = (uint32_t)(d < 0 ? -d : d); if (m > jump) jump = m; }
    }
    *peak = top; *flat = longest; *spike = jump; *rms = sqrt(acc / kept);
}

int main(int argc, char **argv)
{
    static br_part_t saved[BR_PARTS];
    uint64_t lane_energy[4] = {0};
    br_sound_t sound;
#if defined(BR_POLY_TEST_TWO_CORES)
    static int32_t trace[20][BR_BLOCK];
    int parallel = argc > 1 && strcmp(argv[1], "parallel") == 0;
    uint32_t model = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 8u;
    uint32_t selected = argc > 3 ? (uint32_t)strtoul(argv[3], 0, 10) : 0u;
    assert(model < PLAITS_MODEL_COUNT);
    assert(selected < BR_SYNTH_PARTS);
#else
    uint32_t model = 8u;
    uint32_t selected = 0u;
    (void)argc; (void)argv;
#endif
#if FM1_PLAITS_PACK_WAVES
    /* The device decodes the packed Plaits resources (wavetables, sine table, DX7 banks, Braids
     * waves) at boot (platform.c); without it the models that read them render silence. */
    assert(fm1_plaits_waves_init(NULL));
#endif
    configure(BR_KIT_808, 0u);
    br_preset_sound(BR_FIRST_PLAITS + 18u, &sound);
    br_part[0].sound = sound;
    br_part[0].revision += 2u;
    br_snapshot(0u);
    {
        uint32_t full = plaits_compact_state_bytes(15u);
        uint32_t primary_slots = (uint32_t)(sizeof br_dsp / full);
        uint32_t required = primary_slots < BR_POLY_VOICES ? (BR_POLY_VOICES - primary_slots) * full : 0u;
        assert(primary_slots > 0u && required && br_fx_poly_can_loan(0u, required));
        printf("sweep capacity: %u bytes/voice, %u primary voices, %u borrowed bytes\n", full, primary_slots, required);
    }
    /* Entering while a punch effect is active must leave live DSP alone. */
    static uint8_t original_dsp[sizeof br_dsp];
    memcpy(original_dsp, br_dsp, sizeof original_dsp);
    br_punch_effect = 1u;
    assert(!br_poly_scheduler_enter(0u, 1));
    assert(memcmp(original_dsp, br_dsp, sizeof original_dsp) == 0);
    br_punch_effect = 0u;
#if BR_AUDIO_STAGGER_COLD_SYNTH_STARTS
    br_deferred_start[0].valid = 1u;
    assert(!br_poly_scheduler_enter(0u, 1));
    assert(memcmp(original_dsp, br_dsp, sizeof original_dsp) == 0);
    br_deferred_start[0].valid = 0u;
#endif
    br_preset_sound(BR_FIRST_PLAITS + model, &sound);
    sound.attack = 0u; sound.decay = 0u; sound.sustain = 127u; sound.level = 100u;
    br_part[selected].sound = sound;
    br_part[selected].revision += 2u;
    br_snapshot(selected);
    br_build_effective(selected);
    /* The chord soft knee: unchanged to 24576, then smooth (no step larger than
     * one input step), rising, and never past full scale: no corner, no clip. */
    for (int32_t x = -60000; x < 60000; ++x) {
        int32_t y = br_poly_soft_knee(x), next = br_poly_soft_knee(x + 1);
        assert(y >= -32767 && y <= 32767);
        assert(next >= y && next - y <= 1);
        if (x >= -24576 && x <= 24576) assert(y == x);
        assert(br_poly_soft_knee(-x) == -y);
    }
    assert(br_poly_soft_knee(40960) >= 32766 && br_poly_soft_knee(32767) > 30000);
    for (int32_t x = -200000; x < 200000; ++x) {
        int32_t y = br_poly_wide_knee(x), next = br_poly_wide_knee(x + 1);
        assert(y >= -98304 && y <= 98304 && next >= y && next - y <= 1);
        if (x >= -65536 && x <= 65536) assert(y == x);
    }
    assert(br_poly_wide_knee(131072) == 98304);
    /* A long mono release must not hold the switch for its whole tail:
     * once polyphony is wanted, the selected track's tail fades in 10 ms.
     * The fixture leaves each block's FX stage open; it is closed between blocks. */
#define TAIL_RENDER() do { br_engine_render(bus, BR_BLOCK); \
        br_fx_fusion_cancel(); } while (0)
    memcpy(saved, br_part, sizeof saved);
    br_part[selected].sound.release = 127u;
    br_part[selected].sound.voice_mode = 2u;
    br_part[selected].revision += 2u;
    br_snapshot(selected); br_build_effective(selected);
    br_engine_note_on_tagged(selected, 60u, 100u, 1u);
    for (unsigned block = 0; block < 8u; ++block) TAIL_RENDER();
    assert(br_env[selected].stage != BR_ENV_OFF && br_env[selected].stage != BR_ENV_RELEASE);
    assert(br_poly_transition_needed(selected, 0u) == 0u);   /* the note is held */
    assert(br_poly_tail_cut_part == 255u);
    br_engine_note_off_tagged(selected, 60u, 1u);
    TAIL_RENDER();
    assert(br_env[selected].stage == BR_ENV_RELEASE);
    assert(br_poly_transition_needed(selected, 1u) == 0u);   /* suspended: the tail plays on */
    assert(br_poly_tail_cut_part == 255u);
    for (unsigned block = 0; block < 4u; ++block) TAIL_RENDER();
    assert(br_env[selected].stage == BR_ENV_RELEASE);
    assert(br_poly_transition_needed(selected, 0u) == 0u);
    assert(br_poly_tail_cut_part == selected);
    {
        unsigned blocks = 0u;
        while (br_env[selected].stage != BR_ENV_OFF && blocks < 64u) { TAIL_RENDER(); ++blocks; }
        printf("tail cut: off after %u blocks (%u samples)\n", blocks, blocks * BR_BLOCK);
        assert(blocks * BR_BLOCK <= BR_POLY_TAIL_CUT_SAMPLES + 2u * BR_BLOCK);
    }
    assert(br_poly_transition_needed(selected, 0u) == 1u);
    assert(br_poly_tail_cut_part == 255u);
    memcpy(br_part, saved, sizeof saved);
    br_part[selected].revision += 2u;
    br_snapshot(selected); br_build_effective(selected);
    memcpy(saved, br_part, sizeof saved);
    br_live_count[selected] = 1u;
    assert(br_poly_transition_needed(selected, 0u) == 0u);
    br_live_count[selected] = 0u;
    br_env[selected].stage = BR_ENV_RELEASE;
    assert(br_poly_transition_needed(selected, 0u) == 0u);
    br_env[selected].stage = BR_ENV_OFF;
    br_part[selected].sound.voice_mode = 1u;
    assert(br_poly_transition_needed(selected, 0u) == 0u);
    br_part[selected].sound.voice_mode = 2u;
    br_poly_user_off = 1u;
    assert(br_poly_transition_needed(selected, 0u) == 1u);
    br_part[selected].sound.voice_mode = 0u;
    assert(br_poly_transition_needed(selected, 0u) == 0u);
    br_poly_user_off = 0u;
    uint32_t inactive = (selected + 1u) % BR_SYNTH_PARTS;
    br_env[inactive].stage = BR_ENV_RELEASE;
    assert(br_poly_transition_needed(selected, 0u) == 1u);
    br_env[inactive].stage = BR_ENV_OFF;
    br_quiet_part[selected] = 0u; /* a silent Plaits tail must not delay the handoff */
    assert(!br_poly_scheduler_enter(selected, 0));
    assert(br_poly_transition_needed(selected, 1u) == 0u);
    assert(br_poly_transition_needed(selected, 0u) == 1u);
    assert(br_poly_transition_commit(1u, selected));
    assert(br_poly_mode_active());
    for (uint8_t i = 0; i < 6u; ++i)
        br_engine_note_on_tagged(selected, 60u + i, 100u, i + 1u);
    br_take_events();
    assert(br_poly_scheduler_events.count == 6u);
    for (unsigned block = 0; block < 20u; ++block) {
        assert(br_fx_fusion_prepare(bus, BR_BLOCK));
        br_audio_source_fx_fusion_active = 1u;
        memset(&br_audio_source_queue, 0, sizeof br_audio_source_queue);
        br_audio_source_queue.count = br_poly_scheduler_build_jobs(bus, BR_BLOCK);
        assert(br_audio_source_queue.count == 8u);
        br_audio_source_queue.deadline_tick = 100000u;
        assert(br_audio_worker_source_queue_context_valid(&br_audio_source_queue));
#if defined(BR_POLY_TEST_TWO_CORES)
        if (parallel) {
            pthread_t worker;
            void *result;
            assert(!pthread_create(&worker, 0, poly_worker, 0));
            assert(br_audio_source_queue_run(1u));
            assert(!pthread_join(worker, &result));
            assert((uintptr_t)result == 1u);
        } else
#endif
        assert(br_audio_source_queue_run(1u));
        assert(br_audio_source_queue.completed == 8u);
        assert(br_fx_fusion_complete(bus, BR_BLOCK));
        for (unsigned lane = 0; lane < 4u; ++lane)
            for (unsigned i = 0; i < BR_BLOCK; ++i) {
                int32_t value = br_audio_kit_partial[0][lane][i];
                lane_energy[lane] += value < 0 ? -value : value;
            }
#if defined(BR_POLY_TEST_TWO_CORES)
        memcpy(trace[block], bus[selected], sizeof trace[block]);
#endif
        br_fx_fusion_cancel();
    }
    /* Some reference models are silent with this fixed test control setting.
     * For the audible analog model, each lane must carry real samples. */
    if (model == 8u)
        for (unsigned lane = 0; lane < 4u; ++lane) assert(lane_energy[lane] != 0u);
    br_engine_render(bus, BR_BLOCK);
    assert(br_audio_source_queue.count == 8u && br_audio_source_queue.completed == 8u);
    assert(br_fx_fusion_complete(bus, BR_BLOCK));
    uint64_t mix_energy = 0;
    for (unsigned i = 0; i < BR_BLOCK; ++i)
        mix_energy += bus[selected][i] < 0 ? -bus[selected][i] : bus[selected][i];
    if (model == 8u) assert(mix_energy != 0u);
    br_fx_fusion_cancel();
    br_engine_note_off_tagged(selected, 63u, 4u);
    br_take_events();
    assert(br_poly_scheduler_events.count == 5u);
    assert(br_poly_scheduler.envelope[2].stage != BR_ENV_OFF);
    assert(br_poly_scheduler.envelope[3].stage == BR_ENV_RELEASE);
    br_engine_all_off();
    br_take_events();
    assert(br_poly_scheduler_events.count == 0u);
    for (unsigned i = 0; i < BR_POLY_VOICES; ++i)
        assert(br_poly_scheduler.envelope[i].stage == BR_ENV_RELEASE ||
               br_poly_scheduler.envelope[i].stage == BR_ENV_OFF);
    br_engine_note_on_tagged(selected, 67u, 100u, 19u);
    br_take_events();
    assert(br_poly_scheduler_events.count == 1u);
#if BR_AUDIO_STAGGER_COLD_SYNTH_STARTS
    br_deferred_start[0].valid = 1u;
    br_deferred_start[0].due = 0u;
    br_take_events();
    assert(br_deferred_start[0].valid);
#endif
    br_playing = 1u;
    memset(bus, 0x5a, sizeof bus);
    br_engine_render(bus, BR_BLOCK);
    for (unsigned p = 0; p < BR_PARTS; ++p)
        for (unsigned i = 0; i < BR_BLOCK; ++i) assert(bus[p][i] == 0);
    assert(br_poly_scheduler_waiting);
    br_playing = 0u;
    br_engine_midi_transport(0xfau);
    br_take_events();
    assert(br_poly_scheduler_waiting && br_event_read != br_event_write && !br_playing);
    assert(br_poly_transition_needed(selected, 0u) == 2u);
    assert(!br_poly_scheduler_transport(0));
    assert(br_poly_scheduler_enabled);
    br_audio_source_queue.completed--;
    assert(!br_poly_scheduler_transport(1));
    assert(br_poly_scheduler_enabled);
    br_audio_source_queue.completed++;
    assert(br_poly_scheduler_transport(1));
    assert(!br_poly_scheduler_enabled);
#if BR_AUDIO_STAGGER_COLD_SYNTH_STARTS
    assert(!br_deferred_start[0].valid);
#endif
    br_take_events();
    assert(br_playing && br_event_read == br_event_write);
    br_engine_note_off_tagged(selected, 67u, 19u);
    br_take_events();
    assert(br_poly_scheduler_events.count == 0u && br_live_count[selected] == 0u);
    assert(memcmp(saved, br_part, sizeof saved) == 0);
    for (unsigned p = 0; p < BR_PARTS; ++p)
        assert(br_active_preset[p] == br_cached_sound[p].preset);
    br_engine_render(bus, BR_BLOCK);
    assert(br_audio_source_queue.count == 8u && br_audio_source_queue.completed == 8u);
    assert(br_fx_fusion_complete(bus, BR_BLOCK));
    br_fx_fusion_cancel();
    /* MIDI Stop/Continue retains its frozen step and gates through the loan. */
    configure(BR_KIT_808, 0u);
    br_midi_paused = 1u;
    br_seq_note[0] = 64u; br_seq_tie[0] = 1u; br_seq_gate_frames[0] = 600u;
    br_seq_lane_gate_frames[BR_DRUM_PART][0] = 123u;
    assert(br_poly_transition_needed(0u, 0u) == 1u);
    assert(br_poly_transition_commit(1u, 0u));
    assert(br_poly_scheduler_leave(1));
    assert(br_midi_paused && br_seq_note[0] == 64u && br_seq_tie[0] == 1u &&
           br_seq_gate_frames[0] == 600u && br_seq_lane_gate_frames[BR_DRUM_PART][0] == 123u);
    br_engine_midi_transport(0xfbu);
    br_take_events();
    assert(br_playing && !br_midi_paused && br_seq_note[0] == 64u);
    /* The large models borrow inactive tracks' delay buffers. Verify the
     * selected delay survives and no borrowed region is exposed to punch. */
    static const uint8_t large_models[] = {15u, 18u, 19u};
    for (unsigned m = 0; m < sizeof large_models; ++m) {
        for (unsigned part = 0; part < BR_SYNTH_PARTS; ++part) {
            configure(BR_KIT_808, 0u);
            br_preset_sound(BR_FIRST_PLAITS + large_models[m], &sound);
            sound.attack = 0u; sound.decay = 0u; sound.sustain = 127u;
            sound.level = 100u; sound.delay = 0u;
            br_part[part].sound = sound;
            br_part[part].revision += 2u;
            br_snapshot(part);
            br_build_effective(part);
            br_delay_line[part][0] = 12345;
            assert(br_poly_scheduler_enter(part, 1));
            assert(br_fx_poly_loaned && br_fx_delay_alias[part] == br_delay_line[0]);
            uint32_t echo_size = 1u;
            assert(!br_fx_echo_line(3u, &echo_size) && echo_size == 0u);
            for (unsigned i = 0; i < 6u; ++i)
                br_engine_note_on_tagged(part, 60u + i, 100u, i + 1u);
            br_engine_render(bus, BR_BLOCK);
            assert(br_audio_source_queue.count == 8u && br_audio_source_queue.completed == 8u);
            assert(br_fx_fusion_complete(bus, BR_BLOCK));
            br_fx_fusion_cancel();
            assert(br_poly_scheduler_leave(1));
            assert(!br_fx_poly_loaned && br_delay_line[part][0] == 12345);
            for (unsigned p = 0; p < BR_PARTS; ++p) {
                assert(br_fx_delay_alias[p] == br_delay_line[p]);
                if (p != part) assert(br_delay_line[p][0] == 0);
            }
        }
    }
    /* Chords sum with headroom: with the filter bypassed and no chorus a four-note chord is
     * clearly louder than one note and stays under the wide knee's 98304, through the filter
     * as well (the filter wrap). With chorus on it stays within int16: the chorus line
     * stores int16, so there chords are held to full scale (no louder than one loud note). */
    {
        double one, four; uint32_t p1, p4;
        chord_level(0u, 8u, 1u, 64u, 0u, &one, &p1);
        chord_level(0u, 8u, 4u, 64u, 0u, &four, &p4);
#if !defined(BR_POLY_TEST_TWO_CORES)
        printf("chord level, open: 1 note %.1f dB, 4 notes %.1f dB (peaks %u, %u)\n",
               20.0 * log10(one / 32768.0), 20.0 * log10(four / 32768.0), p1, p4);
#endif
        assert(four > one * 1.58 && p4 <= 98304u);
        chord_level(0u, 8u, 1u, 40u, 0u, &one, &p1);
        chord_level(0u, 8u, 4u, 40u, 0u, &four, &p4);
#if !defined(BR_POLY_TEST_TWO_CORES)
        printf("chord level, filter 40: 1 note %.1f dB, 4 notes %.1f dB (peaks %u, %u)\n",
               20.0 * log10(one / 32768.0), 20.0 * log10(four / 32768.0), p1, p4);
#endif
        assert(four > one * 1.58 && p4 <= 98304u);
        chord_level(0u, 8u, 1u, 64u, 64u, &one, &p1);
        chord_level(0u, 8u, 4u, 64u, 64u, &four, &p4);
#if !defined(BR_POLY_TEST_TWO_CORES)
        printf("chord level, chorus: 1 note %.1f dB, 4 notes %.1f dB (peaks %u, %u)\n",
               20.0 * log10(one / 32768.0), 20.0 * log10(four / 32768.0), p1, p4);
#endif
        assert(p4 <= 32767u && p1 <= 32767u);   /* the chorus line is int16: no chord may clip it */
        {
            static const uint8_t cases[][2] = {{127u, 1u}, {127u, 0u}, {90u, 0u}, {64u, 0u}};
            for (uint32_t k = 0; k < sizeof cases / sizeof cases[0]; ++k) {
                uint32_t pk1, fl1, sp1, pk4, fl4, sp4; double r1, r4;
                chord_master(0u, 1u, cases[k][0], cases[k][1], &pk1, &fl1, &sp1, &r1);
                chord_master(0u, 4u, cases[k][0], cases[k][1], &pk4, &fl4, &sp4, &r4);
#if !defined(BR_POLY_TEST_TWO_CORES)
                printf("master, level %u %s: 1 note rms %.0f peak %u; 4 notes rms %.0f peak %u (%+.1f dB) flat %u jump %u/%u\n",
                       cases[k][0], cases[k][1] ? "delay+drive+reverb" : "dry", r1, pk1, r4, pk4,
                       20.0 * log10(r4 / r1), fl4, sp4, sp1);
#endif
                assert(fl4 <= 2u && sp4 <= 4u * (sp1 + 1u));   /* no flat tops, no clicks */
            }
        }
    }
#if defined(BR_POLY_TEST_TWO_CORES)
    assert(fwrite(trace, sizeof trace, 1u, stdout) == 1u);
#else
    puts("polyphonic sources run through the real worker queue");
#endif
}
