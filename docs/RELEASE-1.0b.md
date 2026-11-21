# JamesOS / JamesOS-K 1.0b, build 3124

Build 3124 is an update to JamesOS 1.0b. Chords with Polyphony are fuller and louder, long releases play out in full, and SMK-37 family keyboards get a quicker way back to HOME.

It installs over 1.0b builds 3110, 3120, 3122 and 3123. Projects, sounds and settings carry over.

## Sound

- Chords with Polyphony on are fuller and louder, and still clean. The voices of a chord now add up with more room, and only the loudest peaks are rounded off gently, as on an analog polysynth. Chords of four or more notes no longer drop in level.
- Polyphony now switches on straight away while a long release is still ringing. The old tail fades out in a few milliseconds first.
- Long amp releases play out in full. On the three FM BANK engines the amp RELEASE now sets the tail, where the engine's own envelope used to cut it short. With Polyphony on, a voice with a long release no longer stops after three seconds.

## Screens

- When the arpeggiator is on, the ARP page shows an ARP: ON badge, like the POLY badge on HOME.

## SMK-37 family keyboards

- KNOB BANK returns to HOME from every page and menu, the SOUNDS list included. On the SOUNDS list it no longer jumps to the next category.
- On the step grid of a synth track, hold a step pad and play a key to set that step's note.

JamesOS-K is new. Feedback welcome.

## Known issues

- External clock: if a master starts clock and playback together, JamesOS can join two to three beats late.
- Tracks with different step rates can drift at some tempos.
- Drum retrigger repeats can land up to 4.9 ms early.

## Install

Use the installer at https://gjelly.com/JamesOS. Manuals: [FM-1](https://gjelly.com/JamesOS/manual), [SMK-37 Pro](https://gjelly.com/JamesOS/manual-k).

Builds: FM-1 `FM-1_9673124`, SMK-37 family `SMK-37 Pro_9653124`.
