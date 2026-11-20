#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""JamesOS-K package for an SMK-37 family keyboard running its stock firmware: the JamesOS-K application with
the FM-1's validated 396 MHz boot area and the shared vendor update loader, as the FM-1 from-V15 package does.
The stock firmware stages it and the vendor loader writes the whole image, including the boot area."""
import argparse, hashlib, json
from pathlib import Path
from build_v15_install_package import read_files
from fm1pkg_make import ufw
from validate_package import validate, MVAVE_V15_OTA_SHA256

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--app-package', type=Path, required=True, help='validated JamesOS-K update package')
    ap.add_argument('--boot-package', type=Path, required=True, help='validated FM-1 from-V15 package (396 MHz boot area)')
    ap.add_argument('--boot-sha256', required=True)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    app_raw, boot_raw = a.app_package.read_bytes(), a.boot_package.read_bytes()
    if hashlib.sha256(boot_raw).hexdigest() != a.boot_sha256.lower():
        raise ValueError('boot package SHA-256 differs from the expected source')
    app_info, boot_info = validate(app_raw), validate(boot_raw)
    if not app_info['identity'].startswith('SMK-37 Pro_'):
        raise ValueError('the application package is not a JamesOS-K package')
    if boot_info['loader_identity_source'] != 'pinned V15 loader; hardware-observed':
        raise ValueError('boot package does not contain the pinned V15 loader')
    app = read_files(a.app_package, hashlib.sha256(app_raw).hexdigest())
    boot = read_files(a.boot_package, hashlib.sha256(boot_raw).hexdigest())
    if hashlib.sha256(boot[100]).hexdigest() != MVAVE_V15_OTA_SHA256:
        raise ValueError('unexpected loader in the boot package')
    flash = boot[0][:0x4000] + app[0][0x4000:]
    identity = app_info['identity']
    data = ufw(flash, boot[100], identity)
    result = validate(data, expected_identity=identity)
    a.output.write_bytes(data)
    print(json.dumps({'app': app_info['sha256'], 'boot_source': boot_info['sha256'], 'output': result['sha256'],
                      'identity': identity, 'loader': result['loader_identity'], 'size': len(data)}, indent=2))

if __name__ == '__main__':
    main()
