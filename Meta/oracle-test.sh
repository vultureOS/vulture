#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Mac is the oracle: the same checks, on this Mac and on this system, and every difference explained
# (Tests/Parity/posix-differences.json). Only on a Mac, naturally: elsewhere it says so, and proves nothing.
#
#   Meta/oracle-test.sh [--build-dir DIR]

set -euo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: oracle-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

if [ "$(uname -s)" != Darwin ]; then
    echo "oracle-test: SKIPPED: this needs a Mac to compare with"
    exit 0
fi

out="$build_dir/oracle-test"
mkdir -p "$out"

# ---- On this Mac: compiled for it, from the same source.
xcrun clang++ -std=c++2b -Wno-everything -I "$source_dir/Tests/Userland" "$source_dir/Tests/Userland/posix.cpp" -o "$out/posix-mac"
(cd "$out" && ./posix-mac < /dev/null > mac.out 2>&1) || true
echo "oracle-test: on this Mac ($(sw_vers -productVersion)): $(grep -a 'requirements met' "$out/mac.out")"

# ---- On this system.
"$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --boot-args "utest=posix" --wait-for-exit --timeout 120 \
    --expect "requirements met" > "$out/vulture.out" 2>&1 || true
echo "oracle-test: on this system: $(grep -a 'requirements met' "$out/vulture.out" | tr -d '\r' | head -n 1)"

python3 -I "$source_dir/Meta/compare-oracle.py" posix "$out/mac.out" "$out/vulture.out" "$source_dir/Tests/Parity/posix-differences.json"
echo "oracle-test: PASS"
