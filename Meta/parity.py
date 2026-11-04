#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""How close to a Mac this is: the Parity Ledger.

    Meta/parity.py [--build-dir DIR] [--ledger FILE] [--output FILE] [--check]

Tests/Parity/ledger.json lists what macOS does, a row each. A row with nothing else is something this system doesn't do. A
row may claim "present" or "partial", but only with proof: tests that have to have passed, in <build>/results (see
Meta/record-test.sh). So that the claim can't outlive the fact, the status shown is worked out here, from those results:

    present     claimed, and all of its proof holds
    partial     likewise, for part of it; "missing" says what isn't there
    FAILING     claimed, and some of its proof doesn't hold
    not run     claimed, and the suites that would prove it haven't been run
    absent      not claimed

Proof is a list, each with a "suite" (a ninja target's results) and at most one of:

    "output": TEXT      TEXT is in what that suite said
    "golden": FILE      Tests/UI/Golden/FILE exists, and the suite didn't say it differs
    "chat": [FILE, TEXT]  Tests/UI/FILE has a step with TEXT in it, so the walkthrough still does this

With none, the suite passing is the proof. Writes Documentation/Parity.md and prints the score. Exit status: 0, or 1 if
anything is FAILING (or, with --check, if the document on disk isn't what this makes), or 2 if the ledger is wrong.
"""

import argparse
import json
import pathlib
import subprocess
import sys

SOURCE_DIR = pathlib.Path(__file__).resolve().parent.parent
STATUS_ORDER = ["present", "partial", "FAILING", "not run", "absent"]
STATUS_MARK = {"present": "✅", "partial": "🟡", "FAILING": "❌", "not run": "⚪️", "absent": "—"}


class LedgerError(Exception):
    pass


class Results:
    """What the test suites last said."""

    def __init__(self, directory):
        self.directory = pathlib.Path(directory)
        self._logs = {}
        self._results = {}

    def result(self, suite):
        if suite not in self._results:
            path = self.directory / f"{suite}.result"
            values = None
            if path.exists():
                values = dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)
            self._results[suite] = values
        return self._results[suite]

    def log(self, suite):
        if suite not in self._logs:
            path = self.directory / f"{suite}.log"
            self._logs[suite] = path.read_text(errors="replace") if path.exists() else ""
        return self._logs[suite]

    def suites(self):
        return sorted(path.stem for path in self.directory.glob("*.result"))


def check_proof(proof, results, source_dir):
    """Whether one piece of proof holds: "held", "failed: why", or "not run"."""
    suite = proof.get("suite")
    if not suite:
        raise LedgerError(f"proof without a suite: {proof}")
    kinds = [key for key in ("output", "golden", "chat") if key in proof]
    if len(kinds) > 1:
        raise LedgerError(f"proof of more than one kind: {proof}")
    unknown = set(proof) - {"suite", "output", "golden", "chat"}
    if unknown:
        raise LedgerError(f"proof with {sorted(unknown)}: {proof}")
    result = results.result(suite)
    if result is None:
        return "not run"
    if result.get("status") != "0":
        return f"failed: {suite} failed"
    if "output" in proof and proof["output"] not in results.log(suite):
        return f"failed: {suite} no longer says \"{proof['output']}\""
    if "golden" in proof:
        name = proof["golden"]
        if not (source_dir / "Tests/UI/Golden" / name).exists():
            return f"failed: there is no golden called {name}"
        if f"{name}: differs" in results.log(suite):
            return f"failed: {name} differs"
    if "chat" in proof:
        name, text = proof["chat"]
        path = source_dir / "Tests/UI" / name
        if not path.exists():
            return f"failed: there is no walkthrough called {name}"
        if text not in path.read_text():
            return f"failed: {name} no longer does \"{text}\""
    return "held"


def status_of(row, results, source_dir):
    """A row's status, and why, if it has failed."""
    claim = row.get("claim")
    proof = row.get("proof", [])
    if claim is None:
        if proof:
            raise LedgerError(f"proof without a claim: {row['feature']}")
        return "absent", ""
    if claim not in ("present", "partial"):
        raise LedgerError(f"can't claim \"{claim}\": {row['feature']}")
    if not proof:
        raise LedgerError(f"a claim without proof: {row['feature']}")
    if claim == "partial" and not row.get("missing"):
        raise LedgerError(f"partial, without saying what's missing: {row['feature']}")
    outcomes = [check_proof(piece, results, source_dir) for piece in proof]
    failures = [outcome[len("failed: "):] for outcome in outcomes if outcome.startswith("failed: ")]
    if failures:
        return "FAILING", "; ".join(failures)
    if "not run" in outcomes:
        return "not run", ""
    return claim, ""


def evaluate(ledger, results, source_dir):
    areas = []
    seen = set()
    for area in ledger["areas"]:
        rows = []
        for row in area["rows"]:
            feature = row.get("feature")
            if not feature:
                raise LedgerError(f"a row without a feature in {area['name']}")
            key = (area["name"], feature)
            if key in seen:
                raise LedgerError(f"listed twice: {area['name']}: {feature}")
            seen.add(key)
            status, why = status_of(row, results, source_dir)
            rows.append({**row, "status": status, "why": why})
        areas.append({"name": area["name"], "rows": rows})
    return areas


def count(rows):
    counts = {status: 0 for status in STATUS_ORDER}
    for row in rows:
        counts[row["status"]] += 1
    return counts


def render_bar(fraction, features):
    """The score, as a picture for the README: which then can't say anything but what the last results did."""
    width = 640
    filled = max(14, round(width * fraction / 100))
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} 76" width="{width}" height="76">
  <style>
    text {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif; text-anchor: middle; fill: #1f2328; }}
    .note {{ fill: #59636e; }}
    .track {{ fill: #d1d9e0; }}
    .fill {{ fill: #0a84ff; }}
    @media (prefers-color-scheme: dark) {{
      text {{ fill: #f0f6fc; }}
      .note {{ fill: #9198a1; }}
      .track {{ fill: #3d444d; }}
    }}
  </style>
  <text x="{width / 2:.0f}" y="24" font-size="24" font-weight="600">{fraction:.1f}%</text>
  <rect class="track" x="0" y="36" width="{width}" height="12" rx="6"/>
  <rect class="fill" x="0" y="36" width="{filled}" height="12" rx="6"/>
  <text class="note" x="{width / 2:.0f}" y="70" font-size="13">of {features} features, by the last test results</text>
</svg>
"""


def score(counts):
    """Present counts whole, partial half: a rough figure, and it says so."""
    total = sum(counts.values())
    if total == 0:
        return 0.0
    return 100.0 * (counts["present"] + counts["partial"] / 2) / total


def describe_runs(results, head):
    lines = []
    for suite in results.suites():
        result = results.result(suite)
        commit = result.get("commit", "unknown")
        notes = []
        if head and commit != head:
            notes.append("not the latest commit")
        if result.get("uncommitted") == "1":
            notes.append("with uncommitted changes")
        state = "passed" if result.get("status") == "0" else "FAILED"
        where = f"`{commit}`" + (f" ({', '.join(notes)})" if notes else "")
        lines.append(f"| `{suite}` | {state} | {where} | {result.get('finished', '')} |")
    return lines


def escape(text):
    return text.replace("|", "\\|")


def anchor(name):
    return "".join(character for character in name.lower().replace(" ", "-") if character.isalnum() or character == "-")


def render(areas, results, head):
    every_row = [row for area in areas for row in area["rows"]]
    totals = count(every_row)
    out = []
    out.append("# Parity with macOS")
    out.append("")
    out.append("How close this system is to macOS Sequoia, feature by feature. **Generated by `ninja parity`: don't edit this.** The")
    out.append("features are listed in `Tests/Parity/ledger.json`, and a status is only ever what the last test results support:")
    out.append("nothing here counts as done because someone said so. See [Roadmap](Roadmap.md).")
    out.append("")
    out.append(f"**Score: {score(totals):.1f}%** of {len(every_row)} features (present counts whole, partial half). It's a rough figure:")
    out.append("the rows aren't equally large, and the list is what's been thought of so far.")
    out.append("")
    out.append("| Status | Features |")
    out.append("|---|---|")
    for status in STATUS_ORDER:
        out.append(f"| {STATUS_MARK[status]} {status} | {totals[status]} |")
    out.append("")
    out.append("| Area | Score | ✅ | 🟡 | ❌ | ⚪️ | — |")
    out.append("|---|---|---|---|---|---|---|")
    for area in areas:
        counts = count(area["rows"])
        out.append(f"| [{area['name']}](#{anchor(area['name'])}) | {score(counts):.0f}% | {counts['present']} | {counts['partial']} | {counts['FAILING']} | {counts['not run']} | {counts['absent']} |")
    out.append("")
    for area in areas:
        out.append(f"## {area['name']}")
        out.append("")
        out.append("| Feature | Status | Milestone | Not there yet |")
        out.append("|---|---|---|---|")
        for row in area["rows"]:
            note = row["why"] if row["status"] == "FAILING" else row.get("missing", "")
            out.append(f"| {escape(row['feature'])} | {STATUS_MARK[row['status']]} {row['status']} | {row.get('milestone', '')} | {escape(note)} |")
        out.append("")
    out.append("## What this is based on")
    out.append("")
    out.append("| Suite | Result | Commit | Finished |")
    out.append("|---|---|---|---|")
    out.extend(describe_runs(results, head))
    out.append("")
    return "\n".join(out)


def current_commit(source_dir):
    try:
        return subprocess.run(["git", "-C", str(source_dir), "rev-parse", "--short=12", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def main():
    parser = argparse.ArgumentParser(description="How close to a Mac this is.")
    parser.add_argument("--build-dir", default=str(SOURCE_DIR / "build"))
    parser.add_argument("--ledger", default=str(SOURCE_DIR / "Tests/Parity/ledger.json"))
    parser.add_argument("--output", default=str(SOURCE_DIR / "Documentation/Parity.md"))
    parser.add_argument("--source-dir", default=str(SOURCE_DIR))
    parser.add_argument("--check", action="store_true", help="fail if the document on disk isn't what this makes")
    arguments = parser.parse_args()

    source_dir = pathlib.Path(arguments.source_dir)
    results = Results(pathlib.Path(arguments.build_dir) / "results")
    try:
        ledger = json.loads(pathlib.Path(arguments.ledger).read_text())
        areas = evaluate(ledger, results, source_dir)
    except (LedgerError, json.JSONDecodeError, KeyError) as error:
        print(f"parity: the ledger is wrong: {error}", file=sys.stderr)
        return 2

    document = render(areas, results, current_commit(source_dir))
    output = pathlib.Path(arguments.output)
    out_of_date = not output.exists() or output.read_text() != document
    if arguments.check:
        if out_of_date:
            print(f"parity: {output} isn't what this makes: run ninja parity", file=sys.stderr)
            return 1
    else:
        output.write_text(document)

    rows = [row for area in areas for row in area["rows"]]
    totals = count(rows)
    if not arguments.check:
        bar = output.parent / "Screenshots" / "parity.svg"
        bar.parent.mkdir(parents=True, exist_ok=True)
        bar.write_text(render_bar(score(totals), len(rows)))
    print(f"parity: {score(totals):.1f}% of {len(rows)} features: " + ", ".join(f"{totals[status]} {status}" for status in STATUS_ORDER))
    for area in areas:
        counts = count(area["rows"])
        print(f"parity:   {area['name']}: {score(counts):.0f}% ({counts['present']} present, {counts['partial']} partial, {counts['absent']} absent)")
    failing = [(area["name"], row) for area in areas for row in area["rows"] if row["status"] == "FAILING"]
    for name, row in failing:
        print(f"parity: FAILING: {name}: {row['feature']}: {row['why']}", file=sys.stderr)
    print(f"parity: see {output}")
    return 1 if failing else 0


if __name__ == "__main__":
    sys.exit(main())
