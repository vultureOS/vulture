#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs a test suite and keeps what happened, for Meta/parity.py to go by:
#
#   Meta/record-test.sh BUILD_DIR NAME COMMAND...
#
# <build>/results/NAME.log has everything it said; <build>/results/NAME.result says how it ended, on which commit, and
# when. It passes on the command's output and exit status, so a target that uses this looks as it did without.

set -uo pipefail

build_dir="$1"
name="$2"
shift 2

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
results="$build_dir/results"
mkdir -p "$results"

commit="$(git -C "$source_dir" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
changed=0
if [ -n "$(git -C "$source_dir" status --porcelain 2>/dev/null | grep -v '^??' | head -n 1)" ]; then
    changed=1
fi

# Not a pass until it's said to be: a run that's interrupted leaves this.
printf 'status=interrupted\ncommit=%s\nuncommitted=%s\n' "$commit" "$changed" > "$results/$name.result"
started="$SECONDS"
"$@" 2>&1 | tee "$results/$name.log"
status="${PIPESTATUS[0]}"

{
    echo "status=$status"
    echo "commit=$commit"
    echo "uncommitted=$changed"
    echo "seconds=$((SECONDS - started))"
    echo "finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$results/$name.result"
exit "$status"
