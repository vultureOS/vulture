#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Makes this system's keyboard layouts, from what Tests/Parity/keyboard-oracle.mm measured on a real Mac: which key types what.

    Meta/generate-keyboards.py MEASUREMENTS OUTPUT_DIRECTORY

A layout is a text file, one line for each thing, in this order:

    name NAME
    key CODE PLAIN SHIFT OPTION SHIFT_OPTION CAPS     what it types with each held, as hex UTF-16; - for nothing; dead:HEX for a dead key that's HEX by itself
    compose DEAD_LAYER DEAD_CODE CODE SHIFTED HEX     what that dead key then does to a key
"""

import pathlib
import re
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    layouts = {}
    current = None
    layer = None
    for line in open(sys.argv[1], encoding="utf-8"):
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "layout":
            if len(fields) < 3 or fields[2] == "not":
                current = None
                continue
            current = {"name": fields[2].replace("_", " "), "keys": {}, "compose": []}
            layouts[fields[1]] = current
        elif current is None:
            continue
        elif fields[0] == "layer":
            layer = fields[1]
        elif fields[0] == "compose":
            current["compose"].append((layer, int(fields[1]), int(fields[2]), int(fields[3]), fields[4]))
        elif re.fullmatch(r"\d+", fields[0]):
            code = int(fields[0])
            value = f"dead:{fields[2]}" if fields[1] == "dead" else fields[1]
            current["keys"].setdefault(code, {})[layer] = value

    directory = pathlib.Path(sys.argv[2])
    directory.mkdir(parents=True, exist_ok=True)
    for identifier, layout in layouts.items():
        lines = [f"name {layout['name']}"]
        for code in sorted(layout["keys"]):
            key = layout["keys"][code]
            lines.append(f"key {code} " + " ".join(key.get(name, "-") for name in ("plain", "shift", "option", "shift-option", "caps")))
        for layer, dead, code, shifted, text in layout["compose"]:
            lines.append(f"compose {layer} {dead} {code} {shifted} {text}")
        (directory / f"{identifier}.layout").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
