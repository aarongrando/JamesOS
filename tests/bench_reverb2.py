#!/usr/bin/env python3
"""Build and run the host reverb benchmark (tests/bench_reverb2.c). A tool, not a test. Usage: bench_reverb2.py [--opt O2|O3|Oz] [-D X=1] [blocks]"""
import argparse
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--opt', default='O3'); ap.add_argument('-D', action='append', default=[]); ap.add_argument('blocks', nargs='?', default='4000')
    a = ap.parse_args()
    out = ROOT / 'build/reverb2-bench'; out.mkdir(parents=True, exist_ok=True)
    b = out / 'bench_reverb2'
    subprocess.run(['cc', '-std=gnu11', '-' + a.opt, '-fwrapv', '-ffp-contract=off', '-Wno-unused-function', '-DRV2_LINE_STRUCT=1', '-DREF2_LINE_STRUCT=1',
                    *['-D' + d for d in a.D], 'tests/bench_reverb2.c', '-lm', '-o', str(b)], cwd=ROOT, check=True)
    subprocess.run([str(b), a.blocks], cwd=ROOT, check=True)
if __name__ == '__main__': main()
