#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The kernel, built with UBSan and KASAN-style checks on the heap, so that undefined behavior and
# memory mistakes end in a panic rather than, someday, in something strange. First: it can tell.
# Then: it finds nothing, through the kernel's tests and the whole of the userland's.
#
#   Meta/sanitized-kernel-test.sh [--build-dir DIR]

set -uo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: sanitized-kernel-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

# The same as the superbuild's, plus the one option, in a directory of its own.
out="$build_dir/kernel-sanitized"
cmake -S "$source_dir" -B "$out" -G Ninja --no-warn-unused-cli \
    -DVULTURE_PART=target \
    -DVULTURE_KERNEL_SANITIZERS=ON \
    -DCMAKE_TOOLCHAIN_FILE="$source_dir/Meta/CMake/Toolchain/vulture-aarch64.cmake" \
    -DVULTURE_HOST_TOOLS="$build_dir/host/Tools" -DVULTURE_SIGNING_DIR="$build_dir/signing" > "$out.configure.log" 2>&1 || { tail -5 "$out.configure.log"; exit 1; }
ninja -C "$out" > "$out.build.log" 2>&1 || { grep -E "error|FAILED|undefined" "$out.build.log" | head -20; echo "sanitized-kernel-test: the build failed: see $out.build.log" >&2; exit 1; }

# A copy of the boot volume, with this kernel on it. (The system's files are the usual ones.)
esp="$build_dir/esp-sanitized"
rm -rf "$esp"
cp -R "$build_dir/esp" "$esp"
cp "$out/Kernel/vulture" "$esp/System/Library/Kernels/vulture"
export VULTURE_ESP_DIR="$esp"

status=0
attempt() { # boot-arguments expected-text
    local arguments="$1" expected="$2" output
    output="$("$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --no-disk --boot-args "$arguments" \
        --expect "$expected" --timeout 60 2>&1)"
    if [ $? -eq 0 ]; then
        echo "sanitized-kernel-test: ok: $arguments: $expected"
    else
        echo "sanitized-kernel-test: FAIL: $arguments: wanted \"$expected\"" >&2
        printf '%s\n' "$output" | grep -a "KERNEL PANIC\|Backtrace\|  #[0-9]" | head -12 >&2
        status=1
    fi
}

attempt fault=kasan-overflow "KERNEL PANIC: KASAN: heap buffer overflow: write of 1 bytes"
attempt fault=kasan-overflow-partial "KERNEL PANIC: KASAN: heap buffer overflow: write of 1 bytes"
attempt fault=kasan-use-after-free "KERNEL PANIC: KASAN: use after free: read of 1 bytes"
attempt fault=kasan-double-free "KERNEL PANIC: KASAN: double free"
attempt fault=kasan-memcpy "KERNEL PANIC: KASAN: heap buffer overflow: write of 32 bytes"
attempt fault=ubsan-overflow "KERNEL PANIC: UBSan: signed addition overflow at Kernel/Init.cpp"
attempt fault=ubsan-shift "KERNEL PANIC: UBSan: a shift out of bounds at Kernel/Init.cpp"
attempt fault=ubsan-bounds "KERNEL PANIC: UBSan: an index out of bounds at Kernel/Init.cpp"

# And nothing goes off in ordinary use.
find_nothing() { # boot-arguments expected-text timeout
    local arguments="$1" expected="$2" timeout="$3" output
    output="$("$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --boot-args "$arguments" \
        --expect "$expected" --wait-for-exit --timeout "$timeout" 2>&1)"
    if [ $? -eq 0 ] && ! printf '%s\n' "$output" | grep -aq "KERNEL PANIC"; then
        echo "sanitized-kernel-test: ok: $arguments: nothing found"
    else
        echo "sanitized-kernel-test: FAIL: $arguments" >&2
        printf '%s\n' "$output" | grep -a "KERNEL PANIC\|Backtrace\|  #[0-9]\|FAIL\|Thread " | head -24 >&2
        status=1
    fi
}
find_nothing ktest "ktest: PASS" 300
find_nothing utest "userland: PASS" 500

if [ "$status" -eq 0 ]; then
    echo "sanitized-kernel-test: PASS"
else
    echo "sanitized-kernel-test: FAIL" >&2
fi
exit "$status"
