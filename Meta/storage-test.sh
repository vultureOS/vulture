#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The storage tests, run on a fresh vultureFS disk image:
#
#   Meta/storage-test.sh [--build-dir DIR] [--nvme] persistence
#       Three boots in a row, each writing (with /usr/tests/crash-writer)
#       and powering off; each must find what the one before committed.
#
#   Meta/storage-test.sh [--build-dir DIR] [--nvme] crash [ROUNDS]
#       ROUNDS (default 100) boots that write until the power is cut at a
#       random moment (QEMU killed). After every cut, fsck.vfs must find
#       the disk consistent, and the next boot must find everything that
#       was committed intact and the disk mounting clean.
#
#   --nvme attaches the disk as NVMe rather than virtio-blk.

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$root_dir/build"
disk_interface=()
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --nvme) disk_interface=(--nvme); shift ;;
        *) break ;;
    esac
done
mode="${1:-}"
rounds="${2:-100}"

mkvfs="$build_dir/host/Tools/vfs/mkvfs"
fsck="$build_dir/host/Tools/vfs/fsck.vfs"
archive="$build_dir/esp/System/Library/Kernels/vulture.bootarchive"
for required in "$mkvfs" "$fsck" "$archive"; do
    if [ ! -e "$required" ]; then
        echo "storage-test: $required is missing; build first with: ninja -C $build_dir" >&2
        exit 1
    fi
done

mkdir -p "$build_dir/qemu"
work="$(mktemp -d "$build_dir/qemu/storage.XXXXXX")"
trap 'rm -rf "$work"' EXIT
disk="$work/disk.img"
"$mkvfs" create "$disk" 1024 "$archive" > /dev/null

fail() {
    echo "storage-test: FAIL: $*" >&2
    exit 1
}

check_disk() {
    local report
    if ! report="$("$fsck" "$disk")"; then
        echo "$report" >&2
        fail "fsck after $1"
    fi
}

# Boots once; prints the console. Extra arguments go to run.sh.
boot() {
    "$root_dir/Meta/run.sh" --build-dir "$build_dir" --headless --disk "$disk" ${disk_interface[@]+"${disk_interface[@]}"} "$@" 2>&1
}

committed_in() {
    sed -n 's/.*crash-writer: verified committed iteration \(-\{0,1\}[0-9]*\),.*/\1/p' <<< "$1" | head -n 1
}

case "$mode" in
persistence)
    expected=-1
    for boot_number in 1 2 3; do
        if ! output="$(boot --boot-args persisttest --wait-for-exit --timeout 90 \
            --expect "crash-writer: verified committed iteration $expected, " --expect "crash-writer: done")"; then
            echo "$output" | tail -n 25 >&2
            fail "boot $boot_number"
        fi
        if grep -q "crash-writer: FAIL" <<< "$output"; then
            grep "crash-writer: FAIL" <<< "$output" >&2
            fail "boot $boot_number found damage"
        fi
        check_disk "boot $boot_number"
        echo "storage-test: boot $boot_number found iteration $expected committed by the boot before: OK"
        expected=$((expected + 3))
    done
    echo "storage-test: persistence: PASS"
    ;;
crash)
    last=-1
    for round in $(seq 1 "$rounds"); do
        delay="$((RANDOM % 3)).$(printf '%02d' $((RANDOM % 100)))"
        if ! output="$(boot --boot-args crashtest --timeout 90 --expect "crash-writer: writing" --kill --kill-delay "$delay")"; then
            echo "$output" | tail -n 25 >&2
            fail "round $round did not start writing"
        fi
        if grep -q "crash-writer: FAIL" <<< "$output"; then
            grep "crash-writer: FAIL" <<< "$output" >&2
            fail "round $round found damage"
        fi
        committed="$(committed_in "$output")"
        if [ -z "$committed" ] || [ "$committed" -lt "$last" ]; then
            fail "round $round: committed iteration went from $last to '$committed'"
        fi
        last="$committed"
        check_disk "the power cut of round $round"
        echo "storage-test: round $round: committed iteration $committed intact; power cut after ${delay}s; fsck clean"
    done
    # A last, clean boot checks the final cut too.
    if ! output="$(boot --boot-args persisttest --wait-for-exit --timeout 90 --expect "crash-writer: done")" ||
        grep -q "crash-writer: FAIL" <<< "$output"; then
        echo "$output" | tail -n 25 >&2
        fail "the boot after the last cut"
    fi
    check_disk "the last boot"
    echo "storage-test: crash: $rounds power cuts, all clean: PASS"
    ;;
*)
    sed -n '7,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
    exit 2
    ;;
esac
