#!/usr/bin/env python3
"""Compare the live track/master filter with the frozen per-sample reference, sample for sample and
state for state, on the host; never access hardware. Builds the test for the three output rates
(44.1 kHz, 32 kHz, 48 kHz) because the slew and the coefficient tables differ."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = ['-std=c11', '-O2', '-fwrapv', '-Wall', '-Wno-unused-function', '-Ifirmware/src', '-Ifirmware/gen']
RATES = {
    '44k': ['-DFM1_NATIVE_OUTPUT_32K=0', '-DFM1_NATIVE_OUTPUT_48K=0'],
    '32k': ['-DFM1_NATIVE_OUTPUT_32K=1', '-DFM1_NATIVE_OUTPUT_48K=0'],
    '48k': ['-DFM1_NATIVE_OUTPUT_32K=0', '-DFM1_NATIVE_OUTPUT_48K=1'],
    '44k-fb-mode-1 (tolerance)': ['-DFM1_NATIVE_OUTPUT_32K=0', '-DFM1_NATIVE_OUTPUT_48K=0', '-DBR_FILTER_FB_MODE=1'],
    '44k-cube-split': ['-DFM1_NATIVE_OUTPUT_32K=0', '-DFM1_NATIVE_OUTPUT_48K=0', '-DBR_FILTER_CUBE_SPLIT=1'],
}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reuse-dsp', action='store_true', help='accepted for the suite; this test uses no DSP objects')
    ap.add_argument('--scale', default='1', help='multiply the number of samples (default 1: about 15 million)')
    args = ap.parse_args()
    out = ROOT / 'build/filter-equiv-test'
    out.mkdir(parents=True, exist_ok=True)
    for name, flags in RATES.items():
        binary = out / ('test_filter_equiv_' + name.split()[0])
        r = subprocess.run(['cc', *BASE, *flags, 'tests/test_filter_equiv.c', '-lm', '-o', str(binary)],
                           cwd=ROOT, capture_output=True, text=True)
        (out / ('compile_' + name.split()[0] + '.log')).write_text(r.stdout + r.stderr)
        if r.returncode:
            print(r.stdout + r.stderr, end='')
            raise SystemExit(r.returncode)
        result = subprocess.run([str(binary), args.scale], cwd=ROOT, capture_output=True, text=True)
        (out / ('result_' + name.split()[0] + '.log')).write_text(result.stdout + result.stderr)
        print(name + ': ' + result.stdout + result.stderr, end='')
        result.check_returncode()

if __name__ == '__main__':
    main()
