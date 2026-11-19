#!/usr/bin/env python3
"""Render the worst kit-release cases (today vs the part envelope held open) to 16-bit 44.1 kHz WAV files. A tool, not a test.
Usage: render_kit_release.py OUTDIR   (needs build/host objects; builds tests/probe_kit_release.c through probe_kit_release.py)"""
import array, os, subprocess, sys, wave
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
binary = ROOT / 'build/probe_kit_release-test/probe_kit_release'
env = dict(os.environ, KIT_RELEASE_OUT=str(out))
subprocess.run([str(binary)], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL)
for raw in sorted(out.glob('*.raw')):
    a = array.array('i'); a.frombytes(raw.read_bytes())
    s16 = array.array('h', [max(-32768, min(32767, v)) for v in a])
    with wave.open(str(raw.with_suffix('.wav')), 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100); w.writeframes(s16.tobytes())
    raw.unlink()
print('wrote', *sorted(p.name for p in out.glob('*.wav')), sep='\n  ')
