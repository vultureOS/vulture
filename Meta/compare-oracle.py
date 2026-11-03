#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""What the same checks said on a real Mac and on this system, and whether every difference has been explained.

    Meta/compare-oracle.py NAME MAC_OUTPUT THIS_OUTPUT DIFFERENCES.json

Each output has a line for each check: "PREFIX: AREA: ok: WHAT", or "PREFIX: AREA: FAIL (...): WHAT (errno ...)". Fails on
a difference that isn't explained, on an explanation of something that's no longer a difference, and on a check that only
one of them ran: a comparison of different lists isn't one.
"""

import json
import re
import sys

LINE = re.compile(r"^[a-z-]+: ([a-z ]+): (ok|FAIL \([^)]*\)): (.*?)(?: \(errno \d+: [^)]*\))?\r?$")


def read(path):
    checks = {}
    with open(path, errors="replace") as file:
        for line in file:
            match = LINE.match(line.rstrip("\n"))
            if not match:
                continue
            area, outcome, what = match.groups()
            checks[f"{area}: {what}"] = outcome == "ok"
    return checks


def main():
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    name, mac_path, this_path, differences_path = sys.argv[1:]
    mac = read(mac_path)
    this = read(this_path)
    with open(differences_path) as file:
        explained = {entry["check"]: entry for entry in json.load(file)["differences"]}

    problems = 0

    def problem(text):
        nonlocal problems
        problems += 1
        print(f"oracle: {name}: FAIL: {text}", file=sys.stderr)

    if not mac or not this:
        problem(f"nothing to compare: {len(mac)} checks from the Mac, {len(this)} from this system")
        return 1

    for check in sorted(set(mac) ^ set(this)):
        problem(f"only {'the Mac' if check in mac else 'this system'} ran this: {check}")

    both = set(mac) & set(this)
    different = {check for check in both if mac[check] != this[check]}
    agree = len(both) - len(different)
    print(f"oracle: {name}: {len(both)} checks on both: {agree} agree, {len(different)} differ")
    print(f"oracle: {name}: this system meets {sum(this.values())}, the Mac {sum(mac.values())}")

    for check in sorted(different):
        side = "this system meets it, the Mac doesn't" if this[check] else "the Mac meets it, this system doesn't"
        entry = explained.get(check)
        if entry is None:
            problem(f"not explained ({side}): {check}")
            continue
        milestone = f", {entry['milestone']}" if entry.get("milestone") else ""
        print(f"oracle: {name}: explained ({entry['kind']}{milestone}): {check}: {entry['why']}")

    for check in sorted(set(explained) - different):
        problem(f"explained, but no longer a difference: {check}")

    if problems:
        print(f"oracle: {name}: FAIL: {problems} problems", file=sys.stderr)
        return 1
    print(f"oracle: {name}: every difference is explained")
    return 0


if __name__ == "__main__":
    sys.exit(main())
