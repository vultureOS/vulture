#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Where each of the look's tokens came from, and that what says it was measured still is.

Every token in Base/System/Library/Themes (Light.json, Dark.json, Metrics.json) is named by the first rule in
Tests/UI/Measured/sources.json whose pattern it matches, and that rule says what kind of number it is:

    measured   what AppKit gave, read by Meta/Calibration/measure.m (Tests/UI/Measured/appkit.json and controls.json)
    published  a value Apple publishes (the Human Interface Guidelines, or what an API documents)
    fitted     chosen to reproduce something measured, and held to it by a test or a golden
    original   ours, with nothing of Apple's to match: our own art
    estimated  from memory of Sequoia, still to be measured against it

A token no rule names, or a rule that names nothing, is an error: so is a check that no longer holds. A check is what
makes "measured" more than a word: the token's value against the number measure.m wrote down.

    Meta/token-sources.py                       checks, and says how many of each there are
    Meta/token-sources.py --list estimated      and which those are
    Meta/token-sources.py --take measured.json  keeps what's needed of a new measure.sh run in appkit.json
"""

import argparse
import fnmatch
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
THEMES = ROOT / "Base/System/Library/Themes"
MEASURED = ROOT / "Tests/UI/Measured"
FILES = ("Light", "Dark", "Metrics")
KINDS = ("measured", "published", "fitted", "original", "estimated")


def flatten(value, prefix=""):
    if isinstance(value, dict):
        for key, inner in value.items():
            yield from flatten(inner, f"{prefix}.{key}" if prefix else key)
    else:
        yield prefix, value


def tokens():
    found = {}
    for name in FILES:
        for path, value in flatten(json.loads((THEMES / f"{name}.json").read_text())):
            found[f"{name}:{path}"] = value
    return found


def lookup(data, dotted):
    for part in dotted.split("."):
        if not isinstance(data, dict) or part not in data:
            raise KeyError(dotted)
        data = data[part]
    return data


def same(ours, theirs, weights):
    if isinstance(ours, str) and isinstance(theirs, str):
        return ours.lower().lstrip("#") == theirs.lower().lstrip("#")
    if weights is not None:
        # (AppKit's weights are floats, written down as such: 0.4000000059604645 is its 0.4.)
        mapped = weights.get(str(ours))
        return mapped is not None and isinstance(theirs, (int, float)) and abs(mapped - theirs) < 1e-3
    if isinstance(ours, (int, float)) and isinstance(theirs, (int, float)):
        return abs(ours - theirs) < 1e-6
    return ours == theirs


def check(rule_check, all_tokens, appkit):
    """One check: a token against what was measured, as equal, or as one of the pixels of a row of them."""
    token = rule_check["token"]
    if token not in all_tokens:
        return f"{token}: no such token"
    ours = all_tokens[token]
    try:
        theirs = lookup(appkit, rule_check["measured"])
    except KeyError:
        return f"{token}: nothing measured at {rule_check['measured']}"
    if rule_check.get("among"):
        if not isinstance(theirs, list) or not any(same(ours, pixel, None) for pixel in theirs):
            return f"{token}: {ours} is none of what was measured at {rule_check['measured']}"
        return None
    if not same(ours, theirs, rule_check.get("weights")):
        return f"{token}: {ours}, where AppKit's is {theirs}"
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--list", choices=KINDS, help="name each token of this kind")
    parser.add_argument("--take", help="a measure.sh run, to keep in appkit.json (without the controls, kept already)")
    arguments = parser.parse_args()

    if arguments.take:
        measured = json.loads(pathlib.Path(arguments.take).read_text())
        kept = {key: measured[key] for key in ("measuredOn", "colors", "metrics", "chrome")}
        (MEASURED / "appkit.json").write_text(json.dumps(kept, indent=1, sort_keys=True) + "\n")
        print(f"token-sources: kept what {kept['measuredOn']} measured")
        return 0

    sources = json.loads((MEASURED / "sources.json").read_text())
    appkit = json.loads((MEASURED / "appkit.json").read_text())
    all_tokens = tokens()
    problems = []

    rules = sources["rules"]
    for rule in rules:
        if rule.get("source") not in KINDS:
            problems.append(f"a rule says {rule.get('source')!r}, which isn't one of {', '.join(KINDS)}")
    kind_of = {}
    used = [0] * len(rules)
    for token in all_tokens:
        for index, rule in enumerate(rules):
            if any(fnmatch.fnmatchcase(token, pattern) for pattern in rule["tokens"]):
                kind_of[token] = rule["source"]
                used[index] += 1
                break
        else:
            problems.append(f"{token}: no rule says where it came from")
    for index, rule in enumerate(rules):
        if not used[index]:
            problems.append(f"a rule names nothing now: {', '.join(rule['tokens'])}")

    checked = 0
    for rule_check in sources["checks"]:
        problem = check(rule_check, all_tokens, appkit)
        if problem:
            problems.append(problem)
            continue
        if kind_of.get(rule_check["token"]) != "measured":
            problems.append(f"{rule_check['token']}: checked against AppKit, but not said to be measured")
            continue
        checked += 1

    if arguments.list:
        for token in sorted(token for token, kind in kind_of.items() if kind == arguments.list):
            print(token)
        return 0

    counts = {kind: sum(1 for value in kind_of.values() if value == kind) for kind in KINDS}
    print(f"token-sources: {len(all_tokens)} tokens: " + ", ".join(f"{counts[kind]} {kind}" for kind in KINDS))
    print(f"token-sources: {checked} of the measured ones checked against what AppKit gave ({appkit['measuredOn']})")
    for problem in problems:
        print(f"token-sources: FAIL: {problem}", file=sys.stderr)
    if problems:
        return 1
    print("token-sources: every token says where it came from")
    return 0


if __name__ == "__main__":
    sys.exit(main())
