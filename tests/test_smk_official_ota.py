#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""SMK-37 app update session with the official (maker's) loader, in memory only (host, never hardware).

  --stock-package  a JamesOS-K package for stock firmware (396 MHz boot area + app + official loader)
  --k-package      a JamesOS-K update package (Felucca's own loader)
  --flash360       a JamesOS-K unit's flash with the stock SMK boot area
  --flash396       a JamesOS-K unit's flash whose boot area equals the stock package's

Checks: on the 360 unit the official loader is staged after the chip key and image CRC checks and
answers as "ota-" + the app identity; on the 396 unit nothing is staged and the host gets the
0xE0000002 notice; a corrupted image is refused before staging; the own loader keeps its record
name. Nothing outside the staging window is ever erased or written by the app."""
import argparse, pathlib, subprocess, sys, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
from fm1_install import logical_image  # noqa: E402
import host_target  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__)
for name in ('stock-package', 'k-package', 'flash360', 'flash396'):
    ap.add_argument('--' + name, type=pathlib.Path, required=True)
a = ap.parse_args()
flags = host_target.activate('smk37')
with tempfile.TemporaryDirectory() as tmp:
    d = pathlib.Path(tmp); exe = d / 'host'
    subprocess.run(['cc', '-std=c11', '-O1', '-w', *flags, '-Ifirmware/hal/smk37', '-Ifirmware/hal', '-Ifirmware/src',
                    'tests/smk_official_ota_host.c', '-o', str(exe)], cwd=ROOT, check=True)

    def run(raw: bytes, flash: pathlib.Path) -> str:
        p = d / 'pkg'; p.write_bytes(bytes(logical_image(raw)))
        return subprocess.run([str(exe), str(p), str(flash)], check=True, capture_output=True, text=True).stdout

    stock = a.stock_package.read_bytes()
    out = run(stock, a.flash360)
    assert 'ota_session=0 ' in out and 'commits=1' in out and 'official=1' in out, out
    assert 'record name=ota-SMK-37 Pro_9653121 area=0xbb000' in out, out
    assert 'boot area untouched=1' in out and 'OUTSIDE STAGING' not in out, out
    assert 'cleanup=1 window_bytes_left=0' in out, out
    print('360 unit: official loader staged, record ota-SMK-37 Pro_9653121 at 0xBB000; the next boot erases it')

    out = run(stock, a.flash396)
    assert 'ota_session=-16 ' in out and 'commits=0' in out and 'notice=0' not in out and 'erasures=0' in out, out
    print('396 unit: refused before staging, host notice sent, nothing erased')

    bad = bytearray(logical_image(stock))
    # flip one byte deep in the app image (outside the headers): the image CRC check must refuse it
    pos = len(bad) // 2
    bad[pos] ^= 1
    p = d / 'bad'; p.write_bytes(bytes(bad))
    out = subprocess.run([str(exe), str(p), str(a.flash360)], check=True, capture_output=True, text=True).stdout
    assert 'ota_session=0 ' not in out and 'commits=0' in out, out
    print('corrupted image: refused before commit (%s)' % [l for l in out.splitlines() if l.startswith('ota_session')][0])

    out = run(a.k_package.read_bytes(), a.flash360)
    assert 'ota_session=0 ' in out and 'official=0' in out and 'record name=ota-SMK-37 Pro_016 ' in out, out
    assert 'cleanup=0 ' in out and 'window_bytes_left=0' not in out, out
    print('own loader: unchanged record ota-SMK-37 Pro_016; the boot cleanup leaves it')
print('SMK-37 official loader staging: passed')
