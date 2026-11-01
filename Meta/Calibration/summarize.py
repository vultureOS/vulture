#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# What Tests/UI/Measured/controls.json keeps of measure.sh's output: sizes and boxes,
# which are numbers about controls, and no rows of pixels.
#
#   Meta/Calibration/summarize.py measured.json > Tests/UI/Measured/controls.json

import json
import sys

measured = json.load(open(sys.argv[1]))
summary = {}
for name, control in sorted(measured["controls"].items()):
    entry = {
        "frame": control["frame"],
        "intrinsic": control["intrinsic"],
        "alignmentInsets": control["alignmentInsets"],
    }
    for look in ("light-1x", "light-2x", "dark-1x", "dark-2x"):
        entry[look] = {"box": control[look]["box"], "softBox": control[look]["softBox"]}
    summary[name] = entry
print(json.dumps(summary, indent=1, sort_keys=True))
