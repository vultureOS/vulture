#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# What a machine with no serial port can still tell you. On each kind of disk a machine starts from, a whole-disk image
# boots and the kernel panics, on purpose; then the files on its EFI System Partition are read as anything else would read
# them, by the partition table and the directories, and they have to say so. A clean boot after that has to replace the boot
# log and leave the panic alone.
#
#   Meta/disk-log-test.sh [--build-dir DIR]

set -euo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: disk-log-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

out="$build_dir/disk-log-test"
mkdir -p "$out"
read_fat="$source_dir/Meta/read-fat-file.py"
architecture="$(sed -n 's/^VULTURE_ARCH:[A-Z]*=//p' "$build_dir/CMakeCache.txt" 2>/dev/null || true)"
architecture="${architecture:-aarch64}"
boot_file=BOOTAA64.EFI
[ "$architecture" = x86_64 ] && boot_file=BOOTX64.EFI

status=0
ok() { echo "disk-log-test: ok: $*"; }
failed() {
    echo "disk-log-test: FAIL: $*" >&2
    status=1
}

# Room for any boot arguments: the test changes them in place, between boots.
blank_arguments="$(printf '%-200s' '')"

# The image, once: each kind of disk gets a copy. What it boots from has boot arguments, as a person's wouldn't.
esp="$out/esp"
mkdir -p "$esp/EFI/BOOT" "$esp/System/Library/Kernels"
cp "$build_dir/esp/EFI/BOOT/$boot_file" "$esp/EFI/BOOT/"
cp "$build_dir/esp/System/Library/Kernels/vulture" "$build_dir/esp/System/Library/Kernels/vulture.bootarchive" "$esp/System/Library/Kernels/"
printf '%s\n' "$blank_arguments" > "$esp/System/Library/Kernels/boot-args"
"$build_dir/host/Tools/vfs/mkimage" "$out/base.img" 1024 "$esp" "$esp/System/Library/Kernels/vulture.bootarchive" \
    "EFI/BOOT/$boot_file" System/Library/Kernels/vulture System/Library/Kernels/vulture.bootarchive System/Library/Kernels/boot-args > "$out/mkimage.log"

set_arguments() {
    python3 -I "$read_fat" "$1" System/Library/Kernels/boot-args --set "$2"
}

# Copies a file off the disk's EFI System Partition, to look at.
read_log() {
    python3 -I "$read_fat" "$1" "$2" > "$3"
}

# An image's files, before anything's run: what they say they are.
read_log "$out/base.img" Logs/boot.txt "$out/new-boot.txt"
read_log "$out/base.img" Logs/panic.txt "$out/new-panic.txt"
if grep -q "hasn't started anything" "$out/new-boot.txt" && grep -q "hasn't been a kernel panic" "$out/new-panic.txt"; then
    ok "a new disk's logs say that nothing has happened yet"
else
    failed "a new disk's logs aren't what they should be"
fi

panic_message="KERNEL PANIC: Data abort (read)\|KERNEL PANIC: Page fault"

# (QEMU's firmware for AArch64 can't start anything from AHCI. A PC's can.)
devices="virtio nvme usb"
[ "$architecture" = x86_64 ] && devices="$devices ahci"

for device in $devices; do
    image="$out/$device.img"
    cp "$out/base.img" "$image"
    panic_text="$out/$device-panic.txt"
    boot_text="$out/$device-boot.txt"

    # ---- A panic. From a program's system call, so with everything running, and the disk busy if it's going to be.
    set_arguments "$image" "console utest=kernel-fault debugfault=user-read"
    if ! VULTURE_IMAGE_ON="$device" "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --image "$image" \
        --expect "Log: this is in Logs/panic.txt" --timeout 120 > "$out/$device-panic.log" 2>&1; then
        failed "$device: the panic wasn't written; see $out/$device-panic.log"
        continue
    fi
    read_log "$image" Logs/panic.txt "$panic_text"
    if grep -q "$panic_message" "$panic_text" && grep -q "Interrupts:" "$panic_text"; then
        ok "$device: a panic's report is in Logs/panic.txt, with the log before it"
    else
        failed "$device: Logs/panic.txt doesn't have the panic in it; see $panic_text"
    fi
    if grep -q "hasn't been a kernel panic" "$panic_text"; then
        failed "$device: what Logs/panic.txt said before shows through"
    fi
    read_log "$image" Logs/boot.txt "$boot_text"
    if grep -q "Log: keeping this boot's in Logs/boot.txt" "$boot_text"; then
        ok "$device: Logs/boot.txt has this boot in it"
    else
        failed "$device: Logs/boot.txt doesn't have the boot in it; see $boot_text"
    fi

    # ---- A clean boot after it: a new boot log, and the panic left where it is.
    set_arguments "$image" "console"
    if ! VULTURE_IMAGE_ON="$device" "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --image "$image" \
        --chat "$source_dir/Tests/Hardware/disk-log.chat" --timeout 120 > "$out/$device-clean.log" 2>&1; then
        failed "$device: the clean boot didn't get through; see $out/$device-clean.log"
        continue
    fi
    read_log "$image" Logs/panic.txt "$panic_text"
    if grep -q "$panic_message" "$panic_text"; then
        ok "$device: the panic is still there after a clean boot"
    else
        failed "$device: a clean boot lost Logs/panic.txt"
    fi
    read_log "$image" Logs/boot.txt "$boot_text"
    if grep -q "vultureOS userland is up" "$boot_text" && ! grep -q "$panic_message" "$boot_text"; then
        ok "$device: Logs/boot.txt is the new boot's, with nothing of the last"
    else
        failed "$device: Logs/boot.txt isn't the new boot's; see $boot_text"
    fi
done

# ---- A read-only EFI partition, as Meta/run.sh's is: nothing to be done, and nothing to go wrong.
if "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --boot-args "console" --expect "vultureOS userland is up" --timeout 60 > "$out/read-only.log" 2>&1; then
    if grep -q "Log: keeping this boot's" "$out/read-only.log"; then
        failed "it thinks it can write to a read-only partition"
    else
        ok "a partition that can't be written is left alone"
    fi
else
    failed "a read-only partition stopped it booting; see $out/read-only.log"
fi

# What's left of the disks is several GiB of nothing much.
for device in $devices; do
    : > "$out/$device.img"
done
: > "$out/base.img"

if [ "$status" -eq 0 ]; then
    echo "disk-log-test: PASS"
else
    echo "disk-log-test: FAIL" >&2
fi
exit "$status"
