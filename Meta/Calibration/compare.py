#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# How our controls differ from what AppKit drew: measure.sh's rows and boxes against
# ui-catalog's.
#
#   Meta/Calibration/compare.py measured.json rendered.json [name[@look]...]

import json
import sys


def rle(row):
    out = []
    i = 0
    while i < len(row):
        j = i
        while j < len(row) and row[j] == row[i]:
            j += 1
        out.append(row[i] + (f"x{j - i}" if j - i > 1 else ""))
        i = j
    return " ".join(out)


def box_edges(box, inset_x, inset_y):
    return (box["x"], box["y"], box["x"] + box["w"], box["y"] + box["h"])


measured = json.load(open(sys.argv[1]))["controls"]
rendered = json.load(open(sys.argv[2]))
wanted = sys.argv[3:]
worst = {}
for key, ours in sorted(rendered.items()):
    name, look = key.rsplit("-", 2)[0], "-".join(key.rsplit("-", 2)[1:])
    if wanted and name not in wanted and f"{name}@{look}" not in wanted:
        continue
    theirs = measured[name][look]
    a = box_edges(theirs["box"], 0, 0)
    b = box_edges(ours["box"], 0, 0)
    delta = max(abs(p - q) for p, q in zip(a, b))
    soft = max(abs(p - q) for p, q in zip(box_edges(theirs["softBox"], 0, 0), box_edges(ours["softBox"], 0, 0)))
    flag = "" if delta <= 1 else "  <<<"
    print(f"{name:26} {look:9} box theirs {a} ours {b} off {delta}{flag}  soft off {soft}")
    if wanted:
        print("  theirs down  ", rle(theirs["down"])[:1000])
        print("  ours   down  ", rle(ours["down"])[:1000])
        print("  theirs across", rle(theirs["across"])[:1000])
        print("  ours   across", rle(ours["across"])[:1000])
