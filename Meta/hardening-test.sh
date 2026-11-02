#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the kernel does when its own protections are tested: each boot makes one deliberate attempt
# (the boot word fault=KIND), and must end in the panic that catches it. Compare `utest=hardening`,
# which makes the attempts of a program.
#
#   Meta/hardening-test.sh [--build-dir DIR]

set -uo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: hardening-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

status=0
attempt() { # boot-arguments expected-text
    local fault="$1" expected="$2" out
    out="$("$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --no-disk --boot-args "$fault" \
        --expect "$expected" --expect "System halted." --timeout 40 2>&1)"
    if [ $? -eq 0 ]; then
        echo "hardening-test: ok: $fault: $expected"
    else
        echo "hardening-test: FAIL: $fault: wanted \"$expected\"" >&2
        printf '%s\n' "$out" | grep -a "KERNEL PANIC\|Triggering\|deliberate" >&2
        status=1
    fi
}

attempt fault=stack-smash "KERNEL PANIC: stack smashing detected"
attempt fault=text-write "KERNEL PANIC: Data abort (write) at"
attempt fault=rodata-write "KERNEL PANIC: Data abort (write) at"
attempt fault=data-execute "KERNEL PANIC: Instruction abort at"
# The same mistakes with a program's memory, which a program has to ask for. A stray read of user
# memory must not get through (PAN), nor a write; nor may its code run in the kernel (PXN).
attempt "utest=kernel-fault debugfault=user-read" "KERNEL PANIC: Data abort (read) at"
attempt "utest=kernel-fault debugfault=user-write" "KERNEL PANIC: Data abort (write) at"
attempt "utest=kernel-fault debugfault=user-execute" "KERNEL PANIC: Instruction abort at"

# ---- The kernel isn't where it was linked, nor in the same place from one boot to the next.
address_of_fault_function() { # boot-arguments
    "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --no-disk --boot-args "$1" \
        --expect "System halted." --timeout 40 2>&1 | grep -a "Kernel::trigger_deliberate_fault" | head -1 | grep -ao '0xffff[0-9a-f]*'
}
first="$(address_of_fault_function fault=panic)"
second="$(address_of_fault_function fault=panic)"
third="$(address_of_fault_function fault=panic)"
fixed="$(address_of_fault_function "fault=panic nokaslr")"
if [ -z "$first" ] || { [ "$first" = "$second" ] && [ "$second" = "$third" ]; }; then
    echo "hardening-test: FAIL: the kernel didn't move: $first $second $third" >&2
    status=1
else
    echo "hardening-test: ok: the kernel moves: $first $second $third"
fi
case "$fixed" in
    0xffffffff80*) echo "hardening-test: ok: nokaslr leaves it where it was linked: $fixed" ;;
    *) echo "hardening-test: FAIL: with nokaslr it is at $fixed" >&2; status=1 ;;
esac

# ---- What depends on the processor, in emulation if this one hasn't got it. (Apple's hypervisor
# has pointer authentication but not branch targets; software emulation has everything.)
tcg_attempt() { # boot-arguments expected-text
    local fault="$1" expected="$2" out
    out="$("$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --no-disk --tcg --boot-args "$fault" \
        --expect "$expected" --timeout 60 2>&1)"
    if [ $? -eq 0 ]; then
        echo "hardening-test: ok: emulated: $fault: $expected"
    else
        echo "hardening-test: FAIL: emulated: $fault: wanted \"$expected\"" >&2
        printf '%s\n' "$out" | grep -a "KERNEL PANIC\|Protections\|hardening:\|FAIL" | head -20 >&2
        status=1
    fi
}
tcg_attempt fault=pac-return "KERNEL PANIC"
tcg_attempt fault=bti-branch "KERNEL PANIC: Branch target exception"
tcg_attempt fault=panic "Protections: PAN, PAC, BTI"
tcg_attempt utest=hardening "hardening: a changed return address was caught (PAC)"
tcg_attempt utest=hardening "hardening: a jump into a function was caught (BTI)"

if [ "$status" -eq 0 ]; then
    echo "hardening-test: PASS"
else
    echo "hardening-test: FAIL" >&2
fi
exit "$status"
