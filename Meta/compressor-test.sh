#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Memory compression where it counts: on small machines, with programs using more than there is. And
# with nothing that will compress, where it has to come to an end without taking the system with it.
#
#   Meta/compressor-test.sh [--build-dir DIR]

set -uo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: compressor-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

status=0
for memory in 1G 512M; do
    output="$("$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --no-disk --memory "$memory" --boot-args utest=compressor \
        --expect "userland: PASS" --wait-for-exit --timeout 300 2>&1)"
    if [ $? -eq 0 ] && ! printf '%s\n' "$output" | grep -aq "KERNEL PANIC"; then
        printf '%s\n' "$output" | grep -a "^compressor: " | sed "s/^compressor: /compressor-test: $memory: /"
        echo "compressor-test: ok: $memory"
    else
        echo "compressor-test: FAIL: $memory" >&2
        printf '%s\n' "$output" | grep -a "KERNEL PANIC\|Backtrace\|  #[0-9]\|CHECK\|compressor\|Compressor\|out of memory" | head -30 >&2
        status=1
    fi
done
if [ "$status" -eq 0 ]; then
    echo "compressor-test: PASS"
else
    echo "compressor-test: FAIL" >&2
fi
exit "$status"
