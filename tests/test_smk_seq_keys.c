#define DINGHY_HOST 1
#define DINGHY_SMK_PREVIEW 1
#define main host_unused_main
#include "../firmware/src/dinghy.c"
#undef main
#include <assert.h>
#include <string.h>

int main(void)
{
    uint32_t held = 0u;
    uint8_t velocity[27]; memset(velocity, 100, sizeof velocity);
    br_app_init();
    br_ui.part = 0u; br_ui.page = BR_PAGE_SEQ; br_ui.screen[BR_PAGE_SEQ] = 0u;
    br_smk_seq_keys(&held, 3u, 3u, 1u, velocity);
    assert(held == 3u && br_ui.held_note[37] == 54u && br_ui.held_note[38] == 55u);
    br_key_up(0u);
    assert(br_ui.held_note[37] == 54u);
    br_smk_seq_keys(&held, 2u, 0u, 1u, velocity);
    assert(!br_ui.held_note[37] && br_ui.held_note[38] == 55u);
    br_smk_seq_keys(&held, 2u, 2u, 1u, velocity);
    assert(br_ui.held_note[38] == 55u);
    br_smk_seq_keys(&held, 0u, 0u, 0u, velocity);
    assert(!held && !br_ui.held_note[38]);
    br_smk_pedal_set(1u);
    br_smk_seq_keys(&held, 1u, 1u, 1u, velocity);
    br_smk_seq_keys(&held, 0u, 0u, 1u, velocity);
    assert(!br_ui.held_note[37] && br_ui.sustain_note[37] == 54u);
    br_smk_pedal_set(0u);
    assert(!br_ui.sustain_note[37]);
    /* Compact note storage must retain the full MIDI range, including 127. */
    br_play_key(37u, 1, 0u, 127u, 100u);
    assert(br_ui.held_note[37] == 128u);
    br_smk_pedal_set(1u); br_key_up(37u);
    assert(br_ui.sustain_note[37] == 128u);
    br_smk_pedal_set(0u);
    assert(!br_ui.sustain_note[37]);
    br_smk_pedal_set(1u); br_hold_on = 1u;
    br_smk_seq_keys(&held, 1u, 1u, 1u, velocity);
    br_smk_seq_keys(&held, 0u, 0u, 1u, velocity);
    br_hold_toggle(); /* Releasing HOLD while the pedal is down must sustain. */
    assert(br_ui.sustain_note[37]);
    br_smk_pedal_set(0u);
    assert(!br_ui.sustain_note[37]);

    /* A key pressed while a tonal step pad is held sets that step's pitch (FM-1 A10). */
    br_ui.octave = 0;
    br_step_trigger_toggle(0u, 3u, 0u, 0u, 0u);
    assert(br_step_active(0u, 3u, 0u, 0u));
    br_ui.step_press_key = 40u; br_ui.step_press_part = 0u; br_ui.step_press_step = 3u;
    br_ui.step_press_lane = 0u; br_ui.step_press_kit = 0u; br_ui.step_edit_active = 0u;
    br_smk_step_pitch_key(4u);
    assert(br_step_note_get(0u, 3u, 0u) == 57u && br_ui.step_edit_active);
    br_ui.step_press_kit = 1u; br_smk_step_pitch_key(6u);            /* kit steps ignore it */
    assert(br_step_note_get(0u, 3u, 0u) == 57u);
    br_ui.step_press_kit = 0u; br_ui.step_press_step = 4u; br_ui.step_edit_active = 0u;
    br_smk_step_pitch_key(6u);                                        /* empty steps ignore it */
    assert(!br_step_active(0u, 4u, 0u, 0u) && !br_ui.step_edit_active);
    br_ui.step_press_key = BR_STEP_NO_PRESS; br_smk_step_pitch_key(6u); /* nothing held */
    assert(br_step_note_get(0u, 3u, 0u) == 57u);
    return 0;
}
