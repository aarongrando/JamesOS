#!/usr/bin/env python3
"""Build and run the idle-stage probe (a tool, not a test) on the host; never access hardware.
Usage: probe_idle_stages.py [--reuse-dsp] [blocks] [decay_blocks] [note_scenario 0|1]
Host DSP objects come from tools/build_braids_host.sh (host compiler only)."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FLAGS = [
    '-std=c11', '-O2', '-fwrapv', '-Wno-unused-function',
    '-DFM1_NATIVE_OUTPUT_32K=0', '-DFM1_NATIVE_OUTPUT_48K=0',
    '-DBR_AUDIO_INTERNAL_BLOCK160=0', '-DBR_AUDIO_INTERNAL_BLOCK256=1',
    '-DPLAITS_INTERNAL_SAMPLE_RATE=44100', '-DFM1_PLAITS_SOURCE_RATE_32K=0',
    '-DFM1_PLAITS_SOURCE_RATE_44100=1', '-DFM1_PLAITS_SOURCE_RATE_38400=0',
    '-DFM1_PLAITS_SOURCE_RATE_48000=0', '-DFM1_PLAITS_SRC_LAGRANGE4=0',
    '-DFM1_PLAITS_SRC_FLOAT_PHASE_FRACTION=0', '-DFM1_PLAITS_PHASE_LUT=0',
    '-DBR_FX_RATE_SIGNED_441_RECIP=0', '-DBR_FX_TAPE_WOW_DELTA_LUT=0',
    '-DBR_AUDIO_FOREGROUND_FIXED8_PLAN=0',
    '-DBR_MIDI_CLOCK_EXPERIMENT=1', '-DBR_MODEL_MOD_EXPERIMENT=1', '-DBR_ADAPTIVE_POLY=1', '-DBR_CAP_POLY=1',
    '-DFM1_PATTERN_AUDIO_IRQ_PROTOTYPE=1', '-Ifirmware/src', '-Ifirmware/gen',
]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reuse-dsp', action='store_true')
    ap.add_argument('args', nargs='*')
    args = ap.parse_args()
    out = ROOT / 'build/probe-idle-stages'
    out.mkdir(parents=True, exist_ok=True)
    with (out / 'compile.log').open('w') as log:
        if not args.reuse_dsp:
            subprocess.run(['sh', 'tools/build_braids_host.sh'], cwd=ROOT,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        objects = sorted((ROOT / 'build/host/braids').glob('*.o'))
        objects += sorted((ROOT / 'build/host/plaits_reference').glob('*.o'))
        if not objects:
            raise RuntimeError('No host DSP objects; run without --reuse-dsp')
        planner = out / 'planner.o'
        subprocess.run(['cc', *FLAGS, '-c', 'firmware/src/audio_fixed8_plan_unit.c',
                        '-o', str(planner)], cwd=ROOT, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
        binary = out / 'probe_idle_stages'
        subprocess.run(['c++', '-x', 'c', *FLAGS, 'tests/probe_idle_stages.c',
                        '-x', 'none', str(planner), *map(str, objects),
                        '-o', str(binary)], cwd=ROOT, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(binary), *args.args], cwd=ROOT, capture_output=True, text=True)
    (out / 'result.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    result.check_returncode()

if __name__ == '__main__':
    main()
