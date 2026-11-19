#!/usr/bin/env python3
"""Byte-compare the Plaits hi-hat engine with its exact shortcuts on and off (tests/test_hihat_exact.cc).

    python3 tests/run_hihat_exact.py [--full [--part I/N]] [--rate 44100|32000|both]
                                     [--flags strict|device|device-scalar|both] [--measure]

One binary holds two builds of the engine (tests/hihat_exact_harness.cc): the tree's defaults, and a reference
with every FM1_PLAITS_HIHAT_* exact shortcut set to 0. Every output float and every state variable is compared
after every render call over the sweep described in the test source. Default: a thin slice of the sweep at the
device's 44.1 kHz source rate with the host golden-test flags (-O2 -ffp-contract=off). --rate 32000 is the host
tests' source rate; --flags device uses the device's DSP flags (-O3 and the relaxed floating-point flags) on the
host compiler (device-scalar: the same with the auto-vectorizers off); --full runs the whole sweep.

The pass criterion is the strict flag set. With -fassociative-math the current host compiler re-orders float
expressions differently as soon as a function's source changes, so there the two FM1_PLAITS_HIHAT_LOCAL_*
switches are not bit-identical (--measure: at most 0.02 LSB of 16-bit output); the oscillator-bank switches
alone are (add --cur-define FM1_PLAITS_HIHAT_LOCAL_SVF_COEFFICIENTS=0 --cur-define FM1_PLAITS_HIHAT_LOCAL_STATE=0).
The device build's assembly evaluates in source order.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMMON = ['-std=c++14', '-DTEST', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics',
          '-Dstmlib=plaits_stmlib', '-Ifirmware/plaits', '-Ifirmware/src', '-Ifirmware/plaits/upstream',
          '-Ifirmware/braids/upstream']
FLAGSETS = {
    'strict': ['-O2', '-ffp-contract=off'],
    'device': ['-O3', '-ffp-contract=fast', '-fno-signed-zeros', '-fno-trapping-math', '-fassociative-math'],
    'device-scalar': ['-O3', '-ffp-contract=fast', '-fno-signed-zeros', '-fno-trapping-math',
                      '-fassociative-math', '-fno-vectorize', '-fno-slp-vectorize'],
}
RATES = {
    '32000': [],
    '44100': ['-DPLAITS_INTERNAL_SAMPLE_RATE=44100', '-DFM1_PLAITS_SOURCE_RATE_32K=0',
              '-DFM1_PLAITS_SOURCE_RATE_44100=1', '-DFM1_PLAITS_SRC_LAGRANGE4=0',
              '-DFM1_PLAITS_SRC_FLOAT_PHASE_FRACTION=0', '-DFM1_PLAITS_RATE_TIME_CALIBRATION=1',
              '-DFM1_PLAITS_HIHAT_RATE_TIME_CALIBRATION=1'],
}
# The switches under test: the reference build compiles each of them out.
SWITCHES = ['FM1_PLAITS_HIHAT_RINGMOD_CONSTANT_SQUARE', 'FM1_PLAITS_HIHAT_RINGMOD_STEADY_SAW',
            'FM1_PLAITS_HIHAT_LOCAL_SVF_COEFFICIENTS', 'FM1_PLAITS_HIHAT_LOCAL_STATE',
            'FM1_PLAITS_HIHAT_RINGMOD_PHASE_CLOSED_FORM']
SHARED_SOURCES = ['firmware/plaits/upstream/stmlib/dsp/units.cc', 'firmware/plaits/upstream/stmlib/utils/random.cc']


def run(rate, flagset, args):
    out = ROOT / 'build/hihat-exact' / f'{rate}-{flagset}'
    out.mkdir(parents=True, exist_ok=True)
    flags = COMMON + RATES[rate] + FLAGSETS[flagset]
    objects = []
    for src in SHARED_SOURCES:
        obj = out / (Path(src).stem + '.o')
        subprocess.run(['c++', *flags, '-c', src, '-o', str(obj)], cwd=ROOT, check=True)
        objects.append(obj)
    for prefix, extra in (('cur', ['-DFM1_PLAITS_HIHAT_STATS=1'] + ['-D' + d for d in args.cur_define]),
                          ('ref', ['-Dplaits=plaits_ref'] + [f'-D{s}=0' for s in SWITCHES])):
        obj = out / f'harness_{prefix}.o'
        subprocess.run(['c++', *flags, f'-DHIHAT_HARNESS_PREFIX={prefix}', *extra, '-c',
                        'tests/hihat_exact_harness.cc', '-o', str(obj)], cwd=ROOT, check=True)
        objects.append(obj)
    binary = out / 'test_hihat_exact'
    subprocess.run(['c++', *flags, 'tests/test_hihat_exact.cc', *map(str, objects), '-o', str(binary)],
                   cwd=ROOT, check=True)
    argv = [] if args.full else ['--quick']
    if args.measure:
        argv.append('--measure')
    if args.part:
        argv += ['--part', *args.part.split('/')]
    print(f'[{rate} Hz, {flagset} flags{", part " + args.part if args.part else ""}]', flush=True)
    return subprocess.run([str(binary), *argv], cwd=ROOT).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--full', action='store_true',
                    help='the whole sweep (about 460 million frames per rate and flag set) instead of the thin '
                         'slice the suite runs (about 107 million)')
    ap.add_argument('--measure', action='store_true',
                    help='report the largest output difference in 16-bit LSBs instead of failing on a mismatch')
    ap.add_argument('--cur-define', action='append', default=[], metavar='NAME=VALUE',
                    help='extra define for the build under test (experiments)')
    ap.add_argument('--part', default='', metavar='I/N', help='with --full: only cases I modulo N')
    ap.add_argument('--rate', choices=['44100', '32000', 'both'], default='44100')
    ap.add_argument('--flags', choices=['strict', 'device', 'device-scalar', 'both'], default='strict')
    args = ap.parse_args()
    rates = ['44100', '32000'] if args.rate == 'both' else [args.rate]
    flagsets = ['strict', 'device'] if args.flags == 'both' else [args.flags]
    status = 0
    for flagset in flagsets:
        for rate in rates:
            status |= run(rate, flagset, args)
    return 1 if status else 0


if __name__ == '__main__':
    sys.exit(main())
