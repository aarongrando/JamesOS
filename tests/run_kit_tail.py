#!/usr/bin/env python3
"""Run the real-app kit mute click test (a muted lane is silent and does not disturb the others) on the host; never access hardware."""
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
    '-DBR_AUDIO_FOREGROUND_FIXED8_PLAN=0', '-Ifirmware/src', '-Ifirmware/gen',
]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reuse-dsp', action='store_true',
                    help='Use existing host objects only when their C++ sources/headers are unchanged')
    args = ap.parse_args()
    out = ROOT / 'build/kit_tail-test'
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
        binary = out / 'test_kit_tail'
        # The switch is off by default in the firmware; this test keeps the option proven.
        subprocess.run(['c++', '-x', 'c', *FLAGS, '-DBR_KIT_ENV_NO_NOTE_OFF_RELEASE=1', 'tests/test_kit_tail.c',
                        '-x', 'none', str(planner), *map(str, objects),
                        '-o', str(binary)], cwd=ROOT, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
    (out / 'result.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    result.check_returncode()
    # Negative control: with the envelope restart back on (BR_KIT_ENV_NO_NOTE_OFF_RELEASE=0) the tail check must fail.
    neg = out / 'test_kit_tail_restart'
    subprocess.run(['c++', '-x', 'c', *FLAGS, '-DBR_KIT_ENV_NO_NOTE_OFF_RELEASE=0', 'tests/test_kit_tail.c',
                    '-x', 'none', str(planner), *map(str, objects), '-o', str(neg)], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    r = subprocess.run([str(neg)], cwd=ROOT, capture_output=True, text=True)
    if r.returncode == 0:
        raise SystemExit('negative control failed: the check passes with BR_KIT_ENV_NO_NOTE_OFF_RELEASE=0')
    print('negative control: the check fails with the envelope released by a note-off, as it must')

if __name__ == '__main__':
    main()
