#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""The ledger's rules: above all, that a claim doesn't outlive its proof."""

import json
import pathlib
import subprocess
import sys
import tempfile

PARITY = pathlib.Path(__file__).resolve().parent.parent.parent / "Meta/parity.py"
failures = 0


def expect(condition, what):
    global failures
    if condition:
        print(f"ok: {what}")
    else:
        failures += 1
        print(f"FAILED: {what}", file=sys.stderr)


class Fixture:
    def __init__(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        (self.root / "build/results").mkdir(parents=True)
        (self.root / "Tests/UI/Golden").mkdir(parents=True)
        (self.root / "Documentation").mkdir()

    def suite(self, name, status=0, log=""):
        (self.root / f"build/results/{name}.result").write_text(f"status={status}\ncommit=abc\nuncommitted=0\nfinished=now\n")
        (self.root / f"build/results/{name}.log").write_text(log)

    def run(self, rows, *extra):
        (self.root / "ledger.json").write_text(json.dumps({"areas": [{"name": "Area", "rows": rows}]}))
        process = subprocess.run(
            [sys.executable, "-I", str(PARITY), "--source-dir", str(self.root), "--build-dir", str(self.root / "build"), "--ledger", str(self.root / "ledger.json"), "--output", str(self.root / "Documentation/Parity.md"), *extra],
            capture_output=True, text=True)
        document = self.root / "Documentation/Parity.md"
        return process.returncode, process.stdout + process.stderr, document.read_text() if document.exists() else ""


def test_a_row_with_nothing_else_is_absent():
    code, _, document = Fixture().run([{"feature": "X", "milestone": "M99"}])
    expect(code == 0 and "— absent" in document, "a row with nothing else is absent")


def test_a_claim_with_passing_proof():
    fixture = Fixture()
    fixture.suite("test-a", log="the thing works\n")
    code, _, document = fixture.run([{"feature": "X", "claim": "present", "proof": [{"suite": "test-a", "output": "the thing works"}]}])
    expect(code == 0 and "| X | ✅ present |" in document, "a claim whose proof holds is present")


def test_a_failed_suite_fails_the_claim():
    fixture = Fixture()
    fixture.suite("test-a", status=1, log="the thing works\n")
    code, output, document = fixture.run([{"feature": "X", "claim": "present", "proof": [{"suite": "test-a", "output": "the thing works"}]}])
    expect(code == 1 and "| X | ❌ FAILING |" in document and "FAILING" in output, "a suite that failed fails the claim, and the run")


def test_an_interrupted_suite_fails_the_claim():
    fixture = Fixture()
    fixture.suite("test-a", status="interrupted", log="the thing works\n")
    code, _, document = fixture.run([{"feature": "X", "claim": "present", "proof": [{"suite": "test-a"}]}])
    expect(code == 1 and "| X | ❌ FAILING |" in document, "a suite that was interrupted didn't pass")


def test_output_that_has_gone_fails_the_claim():
    fixture = Fixture()
    fixture.suite("test-a", log="something else\n")
    code, _, document = fixture.run([{"feature": "X", "claim": "present", "proof": [{"suite": "test-a", "output": "the thing works"}]}])
    expect(code == 1 and "no longer says" in document, "output that's gone fails the claim, whatever the suite's status")


def test_a_suite_not_run_is_not_a_failure_or_a_pass():
    code, _, document = Fixture().run([{"feature": "X", "claim": "present", "proof": [{"suite": "test-a"}]}])
    expect(code == 0 and "| X | ⚪️ not run |" in document, "a suite that hasn't been run proves nothing")


def test_a_claim_without_proof_is_refused():
    code, output, _ = Fixture().run([{"feature": "X", "claim": "present"}])
    expect(code == 2 and "without proof" in output, "a claim without proof is refused")


def test_partial_has_to_say_what_is_missing():
    fixture = Fixture()
    fixture.suite("test-a")
    code, output, _ = fixture.run([{"feature": "X", "claim": "partial", "proof": [{"suite": "test-a"}]}])
    expect(code == 2 and "what's missing" in output, "partial has to say what's missing")
    code, _, document = fixture.run([{"feature": "X", "claim": "partial", "missing": "the rest", "proof": [{"suite": "test-a"}]}])
    expect(code == 0 and "| X | 🟡 partial |" in document and "the rest" in document, "and then it's shown")


def test_a_golden_must_exist_and_match():
    fixture = Fixture()
    fixture.suite("test-ui", log="")
    code, _, _ = fixture.run([{"feature": "X", "claim": "present", "proof": [{"suite": "test-ui", "golden": "a.png"}]}])
    expect(code == 1, "a golden that doesn't exist fails the claim")
    (fixture.root / "Tests/UI/Golden/a.png").write_text("")
    code, _, _ = fixture.run([{"feature": "X", "claim": "present", "proof": [{"suite": "test-ui", "golden": "a.png"}]}])
    expect(code == 0, "one that does, and matched, holds")
    fixture.suite("test-ui", log="ui-test: a.png: differs from the golden\n")
    code, _, _ = fixture.run([{"feature": "X", "claim": "present", "proof": [{"suite": "test-ui", "golden": "a.png"}]}])
    expect(code == 1, "one that differs fails the claim")


def test_a_walkthrough_must_still_do_it():
    fixture = Fixture()
    fixture.suite("test-ui")
    (fixture.root / "Tests/UI/a.chat").write_text('click button "Save"\n')
    proof = {"suite": "test-ui", "chat": ["a.chat", 'click button "Save"']}
    code, _, _ = fixture.run([{"feature": "X", "claim": "present", "proof": [proof]}])
    expect(code == 0, "a walkthrough that does it holds")
    (fixture.root / "Tests/UI/a.chat").write_text("pause 1\n")
    code, _, _ = fixture.run([{"feature": "X", "claim": "present", "proof": [proof]}])
    expect(code == 1, "one that stopped doing it fails the claim")


def test_the_same_row_twice_is_refused():
    code, output, _ = Fixture().run([{"feature": "X"}, {"feature": "X"}])
    expect(code == 2 and "twice" in output, "the same feature twice is refused")


def test_the_check_notices_a_stale_document():
    fixture = Fixture()
    fixture.suite("test-a", log="")
    rows = [{"feature": "X", "claim": "present", "proof": [{"suite": "test-a"}]}]
    fixture.run(rows)
    code, _, _ = fixture.run(rows, "--check")
    expect(code == 0, "--check passes on what it just made")
    fixture.suite("test-a", status=1)
    code, _, _ = fixture.run(rows, "--check")
    expect(code == 1, "--check fails when the results have moved on")


def test_the_score():
    fixture = Fixture()
    fixture.suite("test-a")
    rows = [
        {"feature": "A", "claim": "present", "proof": [{"suite": "test-a"}]},
        {"feature": "B", "claim": "partial", "missing": "half", "proof": [{"suite": "test-a"}]},
        {"feature": "C"},
        {"feature": "D"},
    ]
    _, output, _ = fixture.run(rows)
    expect("37.5% of 4 features" in output, "present counts whole, partial half")


for name, test in list(globals().items()):
    if name.startswith("test_"):
        test()

print(f"parity tests: {failures} failed")
sys.exit(1 if failures else 0)
