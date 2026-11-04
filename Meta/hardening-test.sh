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

# ---- The seal, against someone with the disk in their hands: a disk with the signed system on it, then changed.
tamper_dir="$build_dir/hardening-test"
rm -rf "$tamper_dir"
mkdir -p "$tamper_dir"
image="$build_dir/esp/System/Library/Kernels/vulture.bootarchive"
# The system partition starts 1 MiB in; its first block is what's signed; then the hashes of the rest.
system_partition=$((1024 * 1024))
make_disk() { # name
    "$build_dir/host/Tools/vfs/mkvfs" create "$tamper_dir/$1.img" 2048 "$image" > /dev/null
    # (run.sh would otherwise bring it up to date with the build, and undo all this.)
    touch "$tamper_dir/$1.img.updated"
}
change_byte_at() { # name offset
    printf 'y' | /bin/dd of="$tamper_dir/$1.img" bs=1 seek="$2" conv=notrunc 2> /dev/null
}
boot_tampered() { # name boot-arguments expected-text
    local name="$1" arguments="$2" expected="$3" output
    output="$("$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --disk "$tamper_dir/$name.img" --boot-args "$arguments" \
        --expect "$expected" --timeout 40 2>&1)"
    if [ $? -eq 0 ]; then
        echo "hardening-test: ok: seal: $name: $expected"
    else
        echo "hardening-test: FAIL: seal: $name: wanted \"$expected\"" >&2
        printf '%s\n' "$output" | grep -a "KERNEL PANIC\|Seal:\|Code signing:\|Root:" | head -6 >&2
        status=1
    fi
}
make_disk signature
change_byte_at signature $((system_partition + 70))
boot_tampered signature "" "KERNEL PANIC: the system's seal is broken: it isn't signed by this system's key"
make_disk size
change_byte_at size $((system_partition + 8))
boot_tampered size "" "KERNEL PANIC: the system's seal is broken: it isn't signed by this system's key"
make_disk hashes
change_byte_at hashes $((system_partition + 4096 + 100))
boot_tampered hashes "" "KERNEL PANIC: the system's seal is broken: it isn't what was signed"
boot_tampered hashes noseal "Seal: off, by the boot word noseal"
# A change that doesn't touch those is only found when it is read: that file stops working, not the system. It doesn't
# matter when: not at first, and not if it's been read before, and been let go of since.
make_disk contents
python3 -I "$source_dir/Meta/flip-block.py" "$tamper_dir/contents.img" file "$build_dir/root/usr/bin/seq" > /dev/null
tamper_chat="$tamper_dir/contents.chat"
cat > "$tamper_chat" <<CHAT
expect login: 
send vulture\r
expect Password:
send vulture\r
expect vulture@vulture ~ % 
send seq 1 2; echo seq-said=\$?\r
expect Seal: block
expect seq-said=
send /bin/ls /System/Library/Seal; echo ls-said=\$?\r
expect Manifest
expect ls-said=0
send cat /usr/bin/uptime > /dev/null; echo uptime-read=\$?\r
expect uptime-read=0
host python3 -I "$source_dir/Meta/flip-block.py" "$tamper_dir/contents.img" file "$build_dir/root/usr/bin/uptime"
send cat /usr/bin/uptime > /dev/null; echo uptime-cached=\$?\r
expect uptime-cached=0
send sudo purge; echo purge-said=\$?\r
expect Password:
send vulture\r
expect purge-said=0
send cat /usr/bin/uptime > /dev/null; echo uptime-evicted=\$?\r
expect Seal: block
expect uptime-evicted=1
send hostname; echo hostname-said=\$?\r
expect hostname-said=0
CHAT
if output="$("$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --disk "$tamper_dir/contents.img" --chat "$tamper_chat" --timeout 90 2>&1)"; then
    echo "hardening-test: ok: seal: a changed block can't be read, even of a file read before it was changed, and the system goes on"
else
    echo "hardening-test: FAIL: seal: contents" >&2
    printf '%s\n' "$output" | tail -12 >&2
    status=1
fi
rm -rf "$tamper_dir"

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
