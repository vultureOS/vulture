#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# oracle.py DBSHELL STATEMENTS: runs the statements through LibDatabase and through this Mac's SQLite, and they have to say
# the same. What's an error has to be one in both, though each may say so in its own words only where they're listed here.

import os
import shutil
import subprocess
import sys
import tempfile


def run(command, statements):
    return subprocess.run(command, input=statements, capture_output=True, text=True, timeout=120)


def main():
    dbshell, path = sys.argv[1], sys.argv[2]
    sqlite = shutil.which('sqlite3') or ('/usr/bin/sqlite3' if os.path.exists('/usr/bin/sqlite3') else None)
    if not sqlite:
        print('oracle: no sqlite3 here to compare with')
        return 0
    with open(path, encoding='utf-8') as handle:
        statements = handle.read()
    with tempfile.TemporaryDirectory() as work:
        ours_run = run([dbshell, os.path.join(work, 'ours.db')], statements)
        theirs_run = run([sqlite, '-batch', os.path.join(work, 'theirs.db')], statements)
    # sqlite3 says errors on stderr; ours, among the answers. As many have to go wrong, and the rest has to line up.
    ours_lines = ours_run.stdout.splitlines()
    ours = [line for line in ours_lines if not line.startswith('Error:')]
    our_errors = len(ours_lines) - len(ours)
    their_errors = sum(1 for line in theirs_run.stderr.splitlines() if 'rror' in line)
    theirs = theirs_run.stdout.splitlines()
    failures = 0
    if our_errors != their_errors:
        print(f'ours failed {our_errors} statements, SQLite {their_errors}')
        print('\n'.join(line for line in ours_lines if line.startswith('Error:')))
        print(theirs_run.stderr)
        failures += 1
    for index in range(max(len(ours), len(theirs))):
        mine = ours[index] if index < len(ours) else '(nothing)'
        real = theirs[index] if index < len(theirs) else '(nothing)'
        if mine != real:
            print(f'line {index + 1}: ours {mine!r}, SQLite {real!r}')
            failures += 1
            if failures > 40:
                break
    print(f'oracle: {len(theirs)} lines of answers and {their_errors} errors from SQLite, {failures} different')
    return 1 if failures else 0


sys.exit(main())
