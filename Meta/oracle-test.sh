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
xcrun clang++ -std=c++2b -Wno-everything -I "$source_dir/Tests/Userland" "$source_dir/Tests/Userland/posix.cpp" -liconv -o "$out/posix-mac"
(cd "$out" && ./posix-mac < /dev/null > mac.out 2>&1) || true
echo "oracle-test: on this Mac ($(sw_vers -productVersion)): $(grep -a 'requirements met' "$out/mac.out")"

# ---- On this system.
"$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --boot-args "utest=posix" --wait-for-exit --timeout 120 \
    --expect "requirements met" > "$out/vulture.out" 2>&1 || true
echo "oracle-test: on this system: $(grep -a 'requirements met' "$out/vulture.out" | tr -d '\r' | head -n 1)"

python3 -I "$source_dir/Meta/compare-oracle.py" posix "$out/mac.out" "$out/vulture.out" "$source_dir/Tests/Parity/posix-differences.json"

# ---- What commands say, the same way: the same program, and the same files for them to work on.
xcrun clang++ -std=c++2b -Wno-everything "$source_dir/Tests/Userland/commands.cpp" -o "$out/commands-mac"
(cd "$out" && ./commands-mac < /dev/null > commands-mac.out 2>&1) || true
"$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --boot-args "utest=commands" --wait-for-exit --timeout 240 \
    --expect "commands: finished" > "$out/commands-this.out" 2>&1 || true
python3 -I "$source_dir/Meta/compare-commands.py" "$out/commands-mac.out" "$out/commands-this.out" "$source_dir/Tests/Parity/command-differences.json"

# ---- Numbers, dates and sorting, by Foundation on this Mac and by LibLocale there, for the locales that system has.
xcrun clang++ -x objective-c++ -std=c++20 -Wno-everything -fobjc-arc "$source_dir/Tests/Parity/locale-oracle.mm" -framework Foundation -o "$out/locale-mac"
(cd "$out" && ./locale-mac < /dev/null > locale-mac.out 2>&1) || true
"$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --boot-args "utest=locale-print" --wait-for-exit --timeout 120 \
    --expect "commands: finished" > "$out/locale-this.out" 2>&1 || true
python3 -I "$source_dir/Meta/compare-commands.py" "$out/locale-mac.out" "$out/locale-this.out" "$source_dir/Tests/Parity/locale-differences.json" --name locale

# ---- The same for the C library's own locales, which a Mac keeps quite apart from all that.
xcrun clang++ -std=c++2b -Wno-everything "$source_dir/Tests/Userland/locale-files.cpp" -o "$out/locale-files-mac"
(cd "$out" && ./locale-files-mac < /dev/null > locale-files-mac.out 2>&1) || true
"$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --boot-args "utest=locale-files" --wait-for-exit --timeout 120 \
    --expect "commands: finished" > "$out/locale-files-this.out" 2>&1 || true
python3 -I "$source_dir/Meta/compare-commands.py" "$out/locale-files-mac.out" "$out/locale-files-this.out" "$source_dir/Tests/Parity/locale-file-differences.json" --name "C locales"

# ---- Shaping: which glyphs, and where, by Core Text and by this system's shaper, for the same text in the same font. Only if the fonts have been fetched: Meta/fetch-fonts.sh.
if [ -d "$build_dir/fonts" ]; then
    xcrun clang++ -x objective-c++ -std=c++20 -Wno-everything -fobjc-arc "$source_dir/Tests/Parity/shaping-oracle.mm" -framework Foundation -framework CoreText -framework CoreGraphics -o "$out/shaping-mac"
    python3 -I "$source_dir/Meta/compare-shaping.py" "$out/shaping-mac" "$build_dir/host/Tests/LibText/shape-print" "$build_dir/fonts" "$source_dir/Tests/Parity/shaping-corpus.txt" "$source_dir/Tests/Parity/shaping-differences.json"
    xcrun clang++ -x objective-c++ -std=c++20 -Wno-everything -fobjc-arc "$source_dir/Tests/Parity/variations-oracle.mm" -framework Foundation -framework CoreText -framework CoreGraphics -o "$out/variations-mac"
    python3 -I "$source_dir/Meta/compare-variations.py" "$out/variations-mac" "$build_dir/host/Tests/LibText/shape-print" "$build_dir/fonts/NotoSans-VF.ttf"
else
    echo "oracle-test: shaping: SKIPPED: the fonts haven't been fetched"
fi
echo "oracle-test: PASS"
