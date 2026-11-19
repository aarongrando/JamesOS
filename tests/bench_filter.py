#!/usr/bin/env python3
"""Build and run the host filter benchmark (tests/bench_filter.c). A tool, not a test: not named run_*.py,
so the suite does not pick it up. Usage: python3 tests/bench_filter.py [--opt O2|Oz|Os] [blocks]"""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--opt', default='O2')
    ap.add_argument('-D', action='append', default=[], help='extra define, e.g. -D BR_FILTER_FB_MODE=1')
    ap.add_argument('blocks', nargs='?', default='20000')
    args = ap.parse_args()
    out = ROOT / 'build/filter-bench'
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'bench_filter'
    subprocess.run(['cc', '-std=c11', '-' + args.opt, '-fwrapv', '-Wno-unused-function',
                    '-DFM1_NATIVE_OUTPUT_32K=0', '-DFM1_NATIVE_OUTPUT_48K=0', *['-D' + d for d in args.D], '-Ifirmware/src', '-Ifirmware/gen',
                    'tests/bench_filter.c', '-lm', '-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary), args.blocks], cwd=ROOT, check=True)

if __name__ == '__main__':
    main()
