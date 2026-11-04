#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""A variable font's glyphs at several settings, by Core Text and by this system: within a unit, each one's advance and bounds.

    Meta/compare-variations.py ORACLE SHAPE_PRINT FONT
"""

import subprocess
import sys

SETTINGS = [(100, 100), (400, 100), (700, 100), (900, 100), (400, 62), (900, 62), (550, 80)]
TEXT = "Hamburgefonstiv&@0123456789"


def parse(line):
    glyphs = []
    for field in line.split(":", 1)[1].split():
        glyph, advance, bounds = field.split(":")
        glyphs.append((int(glyph), int(advance), [int(v) for v in bounds.split(",")]))
    return glyphs


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    oracle, shape_print, font = sys.argv[1:]
    compared = 0
    problems = []
    for weight, width in SETTINGS:
        mac = subprocess.run([oracle, font, str(weight), str(width), TEXT], capture_output=True, text=True).stdout
        ours = subprocess.run([shape_print, font, "--variations", str(weight), str(width), TEXT], capture_output=True, text=True).stdout
        if not mac or not ours:
            problems.append(f"nothing came back at {weight} {width}")
            continue
        for character, a, b in zip(TEXT, parse(mac), parse(ours)):
            compared += 1
            close = a[0] == b[0] and abs(a[1] - b[1]) <= 1 and all(abs(x - y) <= 1 for x, y in zip(a[2], b[2]))
            if not close:
                problems.append(f"{character} at wght {weight}, wdth {width}: the Mac says {a}, this system {b}")
    print(f"oracle: variations: {compared} glyphs at {len(SETTINGS)} settings: {compared - len(problems)} within a unit")
    for problem in problems[:20]:
        print(f"oracle: variations: FAIL: {problem}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
