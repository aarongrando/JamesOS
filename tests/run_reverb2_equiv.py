#!/usr/bin/env python3
"""Compare the live shared reverb with the frozen reference sample for sample and state for state, on the host; never
access hardware. Built with the device's relaxed float flags are not needed: the reverb's per-sample core is integer;
the design runs once per setting in the same compile for both sides."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ['-std=gnu11', '-O2', '-fwrapv', '-ffp-contract=off', '-fno-fast-math', '-Wall', '-Wno-unused-function', '-DRV2_LINE_STRUCT=1', '-DREF2_LINE_STRUCT=1']

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reuse-dsp', action='store_true', help='accepted for the suite runner; unused')
    ap.add_argument('--scale', default='1')
    args = ap.parse_args()
    out = ROOT / 'build/reverb2-equiv'
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'test_reverb2_equiv'
    r = subprocess.run(['cc', *FLAGS, 'tests/test_reverb2_equiv.c', '-lm', '-o', str(binary)], cwd=ROOT, capture_output=True, text=True)
    (out / 'compile.log').write_text(r.stdout + r.stderr)
    if r.returncode:
        print(r.stdout + r.stderr, end=''); raise SystemExit(r.returncode)
    result = subprocess.run([str(binary), args.scale], cwd=ROOT, capture_output=True, text=True)
    (out / 'result.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    result.check_returncode()

if __name__ == '__main__':
    main()
