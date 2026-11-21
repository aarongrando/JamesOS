# JamesOS

<p align="center">
  <img src="assets/jamesos-start.png" alt="JamesOS startup graphic" width="200">
</p>

JamesOS is community firmware for the M-VAVE FM-1, built on Dinghy and the Felucca platform with SLOOP contributions. It adds synth engines, drum kits, sequencing and performance controls. The project is independent of the hardware manufacturer.

JamesOS is a Gjelly project by Aaron Grando (a.k.a. James Os).

## Current release

**JamesOS and JamesOS-K 1.0b, build 3124.** This documentation describes the current published release.

| Product | Hardware | Firmware identity |
| --- | --- | --- |
| JamesOS | M-VAVE FM-1 | `FM-1_9673124` |
| JamesOS-K | M-VAVE SMK-37 Pro | `SMK-37 Pro_9653124` |

- [Install or update](https://gjelly.com/JamesOS/install)
- [FM-1 manual](https://gjelly.com/JamesOS/manual)
- [SMK-37 Pro manual](https://gjelly.com/JamesOS/manual-k)
- [Project Editor](https://gjelly.com/editor) (FM-1 only)
- [Release notes](docs/RELEASE-1.0b.md)
- [Build from source](docs/BUILD.md)

## What it does

- Three synth tracks and a four-lane drum kit.
- 24 Plaits engines and 128 starting sounds.
- Drum kits synthesized with the Plaits percussion models.
- A sequencer with patterns, and a pattern arranger: a timeline of blocks with bar lengths and loops.
- Effects per track (drive, delay and modulation) and a global reverb with a per-track send. Delay times and the reverb pre-delay can follow the tempo.
- Two patch slots per track, LFOs and a global LFO.
- Voice modes (glide, LPG gate) in the HOME deep menu.
- Projects saved in 12 named slots, with import and export through the browser Editor (FM-1 only).
- The HD interface on both products.
- Screen-reader support over USB MIDI.

## Projects and updates

Build 3124 installs over 1.0b builds 3110, 3120, 3122 and 3123; projects, sounds and settings carry over. Back up saved work before changing firmware, and keep the instrument connected throughout installation.

Projects saved by 1.0a require conversion through the Editor. Download and keep the original, then import the converted project into an empty slot. Conversion can change the sound or refuse incompatible data. Do not save over a slot marked OLD FORMAT before preserving its contents. The Editor supports the FM-1 only.

FM-1 and SMK-37 Pro firmware packages are not interchangeable. Use the browser installer to select the package for your device and its installed firmware.

## Accessibility

The [FM-1 Screen Reader](https://charlesvestal.github.io/fm1-reader/) reads page names, selected items, values and messages over USB MIDI. Open it in Chrome or Edge and choose Connect.

## Source layout

- `firmware/`: application, loader, hardware support and synthesis libraries.
- `assets/`: inputs for generated firmware data.
- `tools/`: build and device utilities.
- `tests/`: host regression tests and machine-readable comparison fixtures.
- `docs/`: current release instructions and component notices.

## Licences and credits

JamesOS firmware code is GPL-3.0-only; see `LICENSE`. The platform derives from [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita and Hügelton Instruments, with [SLOOP](https://github.com/isod89/sloop-fm1) contributions, and [Dinghy](https://github.com/zvenson/dinghy) by zvenson. Plaits, Braids and stmlib retain their MIT licences in their upstream directories. JamesOS-K's SMK-37 Pro input and display transport derive from M. Geisler's Felucca SMK-37 port (GPL-3.0-only). Component notices and the JieLi SDK licence are in `docs/legal/`.
