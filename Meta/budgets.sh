#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The performance budgets that take a whole running system to measure: boots it to the login window, then to a desktop with
# nothing else going on, on QEMU with its hypervisor where it has one, at 2560x1600. Prints what's measured beside what it
# should be, for Documentation/Roadmap.md's table. It only fails if something can't be measured: how fast this machine is
# isn't for a test to pass or fail on, and a number over budget says so.
#
#   Meta/budgets.sh [--build-dir DIR]

set -euo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: budgets.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

out="$build_dir/budgets"
mkdir -p "$out"
status=0

# ---- Boot: as a person would, the real system with its boot screen, to the login window.
VULTURE_GPU_WIDTH=2560 VULTURE_GPU_HEIGHT=1600 "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --gui \
    --expect "launch: first window on screen at" --timeout 60 > "$out/boot.log" 2>&1 || status=1
started="$(sed -n 's/.*Time: the kernel started at \([0-9.]*\) on the clock.*/\1/p' "$out/boot.log" | head -n 1)"
shown="$(sed -n 's/.*launch: first window on screen at \([0-9.]*\).*/\1/p' "$out/boot.log" | head -n 1)"

# ---- Everything else, on a desktop with nothing else going on.
VULTURE_GPU_WIDTH=2560 VULTURE_GPU_HEIGHT=1600 "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --fresh-disk --boot-args uibudgets \
    --expect "budgets: done" --timeout 120 > "$out/budgets.log" 2>&1 || status=1

# Cmd-Tab, on the whole desktop: the shell draws its panel.
VULTURE_GPU_WIDTH=2560 VULTURE_GPU_HEIGHT=1600 "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --fresh-disk --boot-args uidesktop \
    --chat "$source_dir/Tests/UI/switcher-budget.chat" --timeout 120 > "$out/switcher.log" 2>&1 || status=1
value_of() {
    { grep -a "budget: $1: " "$out/budgets.log" || true; } | head -n 1 | tr -d '\r' | sed "s/.*budget: $1: //"
}

# Name, what it should be, the number it must be under, what it's measured to be.
row() {
    local name="$1" target="$2" limit="$3" measured="$4" number verdict
    if [ -z "$measured" ] || [ "$measured" = failed ]; then
        echo "budgets: $name: NOT MEASURED (should be $target)" >&2
        status=1
        return
    fi
    number="${measured%% *}"
    verdict="$(awk -v a="$number" -v b="$limit" 'BEGIN { print (a <= b) ? "within budget" : "OVER BUDGET" }')"
    echo "budgets: $name: $measured (should be $target): $verdict"
}

boot=""
if [ -n "$started" ] && [ -n "$shown" ]; then
    boot="$(awk -v a="$shown" -v b="$started" 'BEGIN { printf "%.2f s", a - b }')"
fi
row "boot, handoff to login window" "at most 2 s" 2 "$boot"
row "null syscall" "under 150 ns" 150 "$(value_of "null syscall")"
row "Text Editor cold launch" "under 300 ms" 300 "$(value_of "Text Editor cold launch")"
row "application cold launch (Calculator)" "under 300 ms" 300 "$(value_of "Calculator cold launch")"
row "Text Editor warm launch" "under 300 ms" 300 "$(value_of "Text Editor warm launch")"
row "window drag at 2560x1600" "at least 58 fps" 100000 "$(value_of "drag frames")"
row "input to flush, 99th percentile" "at most 16.7 ms" 16.7 "$(value_of "input to flush p99")"
row "input to flush, worst" "for information" 100000 "$(value_of "input to flush worst")"
row "whole screen, 12 windows and blur" "at least 58 fps" 100000 "$(value_of "whole screen drag frames")"
row "input to flush, whole screen, 99th percentile" "at most 16.7 ms" 16.7 "$(value_of "whole screen input to flush p99")"

# The slowest of five, the first included.
switcher="$({ grep -a "budget: Cmd-Tab appears: " "$out/switcher.log" || true; } | tr -d '\r' | sed 's/.*budget: Cmd-Tab appears: //; s/ ms.*//' | sort -n | tail -n 1)"
row "Cmd-Tab appears, the slowest of five" "at most a frame, 16.7 ms" 16.7 "${switcher:+$switcher ms}"
if [ "$status" -eq 0 ]; then
    echo "budgets: PASS"
else
    echo "budgets: FAIL: something couldn't be measured; see $out" >&2
fi
exit "$status"
