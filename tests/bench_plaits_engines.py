#!/usr/bin/env python3
"""Build and run the host Plaits engine benchmark (tests/bench_plaits_engines.cc). A tool, not a test: not
named run_*.py, so the suite does not pick it up. Host times are relative only.

    python3 tests/bench_plaits_engines.py [--synths] [--strict] [-D NAME=VALUE ...] [--tag NAME]

By default the DSP is built like the device DSP (-O3 with the relaxed floating-point flags, auto-vectorizers
off because the target has no vector unit). --strict builds it like the host golden tests instead
(-O2 -ffp-contract=off). Objects are cached per flag set under build/plaits-bench/<tag>.
"""
import argparse
import hashlib
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMMON = ['-std=c++14', '-DTEST', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics',
          '-Dstmlib=plaits_stmlib', '-Ifirmware/plaits', '-Ifirmware/src', '-Ifirmware/plaits/upstream',
          '-Ifirmware/braids/upstream']
# The Plaits selectors of the device build (build-settings.json compiler_flags): 44.1 kHz source rate.
DEVICE_DEFINES = ['-DPLAITS_INTERNAL_SAMPLE_RATE=44100', '-DFM1_PLAITS_SOURCE_RATE_32K=0',
                  '-DFM1_PLAITS_SOURCE_RATE_44100=1', '-DFM1_PLAITS_SOURCE_RATE_38400=0',
                  '-DFM1_PLAITS_SOURCE_RATE_48000=0', '-DFM1_PLAITS_SRC_LAGRANGE4=0',
                  '-DFM1_PLAITS_SRC_FLOAT_PHASE_FRACTION=0', '-DFM1_PLAITS_PHASE_LUT=0',
                  '-DFM1_PLAITS_RATE_TIME_CALIBRATION=1', '-DFM1_PLAITS_HIHAT_RATE_TIME_CALIBRATION=1',
                  '-DFM1_NATIVE_OUTPUT_32K=0', '-DFM1_NATIVE_OUTPUT_48K=0']
DEVICE_LIKE = ['-O3', '-ffp-contract=fast', '-fno-signed-zeros', '-fno-trapping-math', '-fassociative-math',
               '-fno-vectorize', '-fno-slp-vectorize']
STRICT = ['-O2', '-ffp-contract=off']
BRAIDS_FLAGS = ['-std=c++14', '-O2', '-fwrapv', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics',
                '-Ifirmware/braids/upstream', '-Ifirmware/braids']


def build(tag, flags, jobs=2):
    out = ROOT / 'build/plaits-bench' / tag
    out.mkdir(parents=True, exist_ok=True)
    sources = [ROOT / 'firmware/plaits/plaits_c_api.cc']
    for sub in ('plaits', 'stmlib'):
        sources += sorted((ROOT / 'firmware/plaits/upstream' / sub).rglob('*.cc'))
    sources.append(ROOT / 'firmware/braids/upstream/braids/resources.cc')

    def compile_one(src):
        obj = out / (str(src.relative_to(ROOT)).replace('/', '_') + '.o')
        f = BRAIDS_FLAGS if 'braids' in src.parts else flags
        subprocess.run(['c++', *f, '-c', str(src), '-o', str(obj)], cwd=ROOT, check=True,
                       stderr=subprocess.DEVNULL if 'braids' in src.parts else None)
        return obj

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        objects = list(pool.map(compile_one, sources))
    binary = out / 'bench_plaits_engines'
    subprocess.run(['c++', *flags, 'tests/bench_plaits_engines.cc', *map(str, objects), '-o', str(binary)],
                   cwd=ROOT, check=True)
    return binary


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--synths', action='store_true', help='also time all 24 synth models')
    ap.add_argument('--strict', action='store_true', help='host golden-test flags instead of device-like flags')
    ap.add_argument('-D', action='append', default=[], help='extra define, e.g. -D FM1_PLAITS_HIHAT_CLOSED_FORM=0')
    ap.add_argument('--flush-denormals', action='store_true',
                    help='x86: run with FTZ+DAZ (timing without the host subnormal penalty)')
    ap.add_argument('--tag', default=None, help='object cache directory name')
    args = ap.parse_args()
    flags = COMMON + DEVICE_DEFINES + (STRICT if args.strict else DEVICE_LIKE) + ['-D' + d for d in args.D]
    tag = args.tag or hashlib.sha1(' '.join(flags).encode()).hexdigest()[:10]
    binary = build(tag, flags)
    subprocess.run([str(binary)] + (['--synths'] if args.synths else []) +
                   (['--flush-denormals'] if args.flush_denormals else []), cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
