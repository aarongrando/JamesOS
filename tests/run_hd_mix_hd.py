#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""HD blend arithmetic: hd_mix, hd_blend and the run blenders against the original formula, exhaustively over the channel ranges and every coverage. (tests/test_hd_mix_hd.c; host only, never hardware)."""
from hd_runner import main

if __name__ == '__main__':
    main('hd_mix', __doc__)
