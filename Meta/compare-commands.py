#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""What the same commands said on a real Mac and on this system, and whether every difference has been explained.

    Meta/compare-commands.py MAC_OUTPUT THIS_OUTPUT DIFFERENCES.json [--show] [--name WHAT]

Both outputs come from Tests/Userland/commands.cpp. A difference left after what's machine-specific has been taken out of both fails it, as
does an explanation of what's no longer different. The differences file has, for each command that differs, why:

    {"command": "file of json", "kind": "chosen", "why": "..."}
"""

import json
import re
import sys

BEGIN = re.compile(r"^commands: BEGIN (.*)$")
END = re.compile(r"^commands: END (.*) status=(-?\d+)$")

# What can't be the same, on two machines, on either of them.
NORMALIZE = [
    (re.compile(r"/private/tmp/"), "/tmp/"),
    (re.compile(r"\[\d+\]"), "[PID]"),
    # What Foundation's logging puts in front of everything it says.
    (re.compile(r"(?m)^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ [a-z_]+\[[\d:]+\] "), ""),
]


def read(path):
    outputs = {}
    current = None
    lines = []
    with open(path, errors="replace") as file:
        for raw in file:
            line = raw.rstrip("\r\n")
            begin = BEGIN.match(line)
            if begin:
                current = begin.group(1)
                lines = []
                continue
            end = END.match(line)
            if end and current == end.group(1):
                text = "\n".join(lines)
                for pattern, replacement in NORMALIZE:
                    text = pattern.sub(replacement, text)
                outputs[current] = (text, int(end.group(2)))
                current = None
                continue
            if current is not None:
                lines.append(line)
    return outputs


def main():
    show = "--show" in sys.argv
    arguments = [argument for argument in sys.argv[1:] if argument != "--show"]
    what = "commands"
    if "--name" in arguments:
        at = arguments.index("--name")
        what = arguments[at + 1]
        del arguments[at : at + 2]
    if len(arguments) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    mac = read(arguments[0])
    this = read(arguments[1])
    with open(arguments[2]) as file:
        explained = {entry["command"]: entry for entry in json.load(file)["differences"]}

    problems = []
    same = 0
    different = {}
    for name in sorted(set(mac) | set(this)):
        if name not in mac or name not in this:
            problems.append(f"only one of them ran: {name}")
            continue
        if mac[name] == this[name]:
            same += 1
        else:
            different[name] = (mac[name], this[name])
    for name, (on_mac, on_this) in different.items():
        if name in explained:
            print(f"oracle: {what}: explained ({explained[name]['kind']}): {name}: {explained[name]['why']}")
            continue
        problems.append(f"not explained: {name}")
        if show:
            print(f"--- {name} (Mac, status {on_mac[1]})\n{on_mac[0]}\n--- {name} (this system, status {on_this[1]})\n{on_this[0]}\n")
    for name in explained:
        if name not in different:
            problems.append(f"explained, but it isn't different any more: {name}")
    print(f"oracle: {what}: {same + len(different)} on both: {same} the same, {len(different)} different")
    for problem in problems:
        print(f"oracle: commands: FAIL: {problem}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
