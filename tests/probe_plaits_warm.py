#!/usr/bin/env python3
"""Build and run tests/probe_plaits_warm.cc on the host (a tool, not a test; never accesses hardware).
usage: probe_plaits_warm.py [model] > warm.csv ; then summarise with --summary warm.csv"""
import argparse, csv, collections, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
FLAGS = ['-std=c++14', '-O2', '-DTEST', '-ffp-contract=off', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics',
         '-Dstmlib=plaits_stmlib', '-Ifirmware/plaits', '-Ifirmware/src', '-Ifirmware/plaits/upstream', '-Ifirmware/braids/upstream',
         '-DFM1_NATIVE_OUTPUT_32K=0', '-DFM1_NATIVE_OUTPUT_48K=0']
def summary(path):
    rows = list(csv.DictReader(open(path)))
    ws = sorted({int(r['window_ms']) for r in rows})
    agg = collections.defaultdict(lambda: collections.defaultdict(lambda: dict(mx=0.0, rms=0.0, env=0.0, lev=1.0, att=1.0)))
    floor = collections.defaultdict(lambda: dict(mx=0.0, rms=0.0, env=0.0, lev=1.0, att=1.0))
    worst = lambda cur, new: new if abs(new - 1.0) > abs(cur - 1.0) else cur
    for r in rows:
        m = int(r['model']); w = int(r['window_ms']); v = agg[m][w]
        v['mx'] = max(v['mx'], float(r['max_lsb'])); v['rms'] = max(v['rms'], float(r['rms_lsb'])); v['env'] = max(v['env'], float(r['env_diff']))
        v['lev'] = worst(v['lev'], float(r['level_ratio'])); v['att'] = worst(v['att'], float(r['attack_ratio']))
        f = floor[m]; f['mx'] = max(f['mx'], float(r['floor_max'])); f['rms'] = max(f['rms'], float(r['floor_rms'])); f['env'] = max(f['env'], float(r['floor_env']))
        f['lev'] = worst(f['lev'], float(r['floor_level'])); f['att'] = worst(f['att'], float(r['floor_attack']))
    print('per model (worst over parameter sets, notes, same/different note): sample max/rms LSB, 10 ms envelope diff LSB, level ratio, 30 ms attack ratio')
    for m in sorted(agg):
        f = floor[m]
        print('model %2d  floor(3 s vs 12 s): max %5.0f rms %7.2f env %7.2f lev %.3f att %.3f' % (m, f['mx'], f['rms'], f['env'], f['lev'], f['att']))
        for w in ws:
            v = agg[m][w]
            print('     W=%5d ms: max %5.0f rms %7.2f env %7.2f lev %.3f att %.3f' % (w, v['mx'], v['rms'], v['env'], v['lev'], v['att']))
def main():
    ap = argparse.ArgumentParser(); ap.add_argument('model', nargs='?'); ap.add_argument('--summary')
    a = ap.parse_args()
    if a.summary: return summary(a.summary)
    out = ROOT / 'build/probe-plaits-warm'; out.mkdir(parents=True, exist_ok=True)
    objs = sorted((ROOT / 'build/host/braids').glob('*.o')) + sorted((ROOT / 'build/host/plaits_reference').glob('*.o'))
    subprocess.run(['c++', *FLAGS, 'tests/probe_plaits_warm.cc', *map(str, objs), '-o', str(out / 'probe')], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([str(out / 'probe')] + ([a.model] if a.model else []), cwd=ROOT, check=True)
main()
