#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Master filter cue and filter chart: no message per SELECT detent, one cue 0.8 s after the last change; the chart is redrawn at most every BR_CHART_MS, cheaply, and settles pixel-identical to a full redraw. (tests/test_filter_cue_hd.c; host only, never hardware)."""
from hd_runner import main

if __name__ == '__main__':
    main('filter_cue', __doc__)
