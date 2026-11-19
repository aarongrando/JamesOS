#!/usr/bin/env python3
"""Build tests/render_kit_click.c twice (BR_KIT_ENV_NO_RESTART=0 and 1), render the Plaits and 808 kits, write before/after/diff WAVs
(16-bit, 44.1 kHz, stereo) to the given directory and print the difference figures. A tool, not a test.
Usage: render_kit_click.py OUTDIR"""
import array, math, struct, subprocess, sys, wave
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import run_kit_mute_click as base

def build(define, name):
    out = ROOT / 'build/kit_mute_click-test'
    objects = sorted((ROOT / 'build/host/braids').glob('*.o')) + sorted((ROOT / 'build/host/plaits_reference').glob('*.o'))
    binary = out / name
    subprocess.run(['c++', '-x', 'c', *base.FLAGS, define, 'tests/render_kit_click.c', '-x', 'none', str(out / 'planner.o'), *map(str, objects), '-o', str(binary)],
                   cwd=ROOT, check=True)
    return binary

def render(binary, preset, raw):
    subprocess.run([str(binary), preset, str(raw)], cwd=ROOT, check=True)
    a = array.array('i'); a.frombytes(Path(raw).read_bytes()); return a

def wav(path, samples):
    s16 = array.array('h', [max(-32768, min(32767, v)) for v in samples])
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100); w.writeframes(s16.tobytes())

def main():
    out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    b0 = build('-DBR_KIT_ENV_NO_RESTART=0', 'render_kit_click_0'); b1 = build('-DBR_KIT_ENV_NO_RESTART=1', 'render_kit_click_1')
    sys.path.insert(0, str(ROOT))
    for name, preset in (('plaits', None), ('808', None)):
        pass
    for name in ('plaits', 'braids'):
        # the preset id is the enum value; get it from the host build through a tiny C program is overkill: the runner passes the number
        pid = name
        before = render(b0, pid, out / (name + '_before.raw')); after = render(b1, pid, out / (name + '_after.raw'))
        diff = array.array('i', [y - x for x, y in zip(before, after)])
        wav(out / (name + '_before.wav'), before); wav(out / (name + '_after.wav'), after); wav(out / (name + '_diff.wav'), diff)
        (out / (name + '_before.raw')).unlink(); (out / (name + '_after.raw')).unlink()
        nz = [i // 2 for i, v in enumerate(diff) if v]
        peak = max(abs(v) for v in diff) if diff else 0
        rms = math.sqrt(sum(v * v for v in diff) / len(diff))
        sig = math.sqrt(sum(v * v for v in before) / len(before)) or 1
        print(f'{name}: {len(before)//2} frames; {len(nz)} samples differ; max {peak} LSB, rms {rms:.2f} LSB ({20*math.log10(rms/sig+1e-12):.1f} dB re the signal rms {sig:.0f}); '
              f'first difference at frame {nz[0] if nz else None} ({(nz[0]/44.1 if nz else 0):.0f} ms), last at {nz[-1] if nz else None}')
        # where: group into hit windows
        groups = []
        for f in nz:
            if groups and f - groups[-1][1] < 2000: groups[-1][1] = f
            else: groups.append([f, f])
        print('   differences cluster at (ms):', ', '.join(f'{g[0]/44.1:.0f}-{g[1]/44.1:.0f}' for g in groups))

if __name__ == '__main__': main()
