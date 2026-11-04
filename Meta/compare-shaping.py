#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""What Core Text does with a corpus of text, and what this system's shaper does with the same text in the same font: which glyphs, and where.

    Meta/compare-shaping.py ORACLE SHAPE_PRINT FONT_DIRECTORY CORPUS DIFFERENCES.json [--show]

Any difference has to be explained in the differences file; so does an explanation of what's no longer different.
"""

import json
import subprocess
import sys


def main():
    show = "--show" in sys.argv
    arguments = [argument for argument in sys.argv[1:] if argument != "--show"]
    if len(arguments) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    oracle, shape_print, fonts, corpus, differences = arguments
    groups = {}
    for line in open(corpus, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        font, script, rtl, text = line.split(" ", 3)
        groups.setdefault((font, script, rtl), []).append(text)
    with open(differences, encoding="utf-8") as file:
        explained = {entry["text"]: entry for entry in json.load(file)["differences"]}

    problems = []
    same = 0
    different = set()
    for (font, script, rtl), texts in groups.items():
        path = f"{fonts}/{font}"
        mac = subprocess.run([oracle, path, rtl, *texts], capture_output=True, text=True)
        ours = subprocess.run([shape_print, path, script, rtl, *texts], capture_output=True, text=True)
        if mac.returncode != 0 or ours.returncode != 0:
            problems.append(f"{font}: the Mac said {mac.returncode}, this system {ours.returncode}: {ours.stderr[-200:]}")
            continue
        for text, on_mac, on_this in zip(texts, mac.stdout.splitlines(), ours.stdout.splitlines()):
            if on_mac == on_this:
                same += 1
                continue
            different.add(text)
            if text not in explained:
                problems.append(f"not explained: {font}: {text}")
                if show:
                    print(f"--- {font} {text}\n   mac {on_mac.split(':', 1)[1]}\n  this {on_this.split(':', 1)[1]}")
    for text, entry in explained.items():
        if text in different:
            print(f"oracle: shaping: explained ({entry['kind']}): {text}: {entry['why']}")
        else:
            problems.append(f"explained, but it isn't different any more: {text}")
    print(f"oracle: shaping: {same + len(different)} on both: {same} the same, {len(different)} different")
    for problem in problems:
        print(f"oracle: shaping: FAIL: {problem}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
