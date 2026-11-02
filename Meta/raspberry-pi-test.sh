#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# What can be known about a Raspberry Pi without one: QEMU's Pi 4, started from kernel8.img. It's what this kernel
# uses there and nowhere else (the GICv2, the spin table, the firmware's mailbox and framebuffer, the watchdog, the
# SD card), on a processor with none of the newer protections. What it can't show is a real Pi's own firmware,
# device tree, EMMC2 controller or USB: see Documentation/RaspberryPi.md.
#
#   Meta/raspberry-pi-test.sh [--build-dir DIR]

set -uo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: raspberry-pi-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

build_dir="$(cd "$build_dir" && pwd)"
out="$build_dir/raspberry-pi-test"
rm -rf "$out"
mkdir -p "$out"
status=0

run() {
    "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --raspberry-pi "$@"
}

ok() { echo "raspberry-pi-test: ok: $*"; }
failed() { echo "raspberry-pi-test: FAIL: $*" >&2; status=1; }

# ---- The kernel: every processor, and its own tests, which run them all.
if run --boot-args ktest --fresh-disk --expect "4 of 4 processors online" --expect "ktest: PASS" --timeout 120 > "$out/kernel.log" 2>&1; then
    ok "the kernel's tests, on all four processors"
else
    failed "the kernel's tests; see $out/kernel.log"
fi

# ---- The display: the login window, as it should look, with its pointer drawn by the kernel.
if VULTURE_SCREENSHOT_PREFIX="$out/" run --gui --fresh-disk --chat "$source_dir/Tests/UI/raspberry-pi.chat" --timeout 120 > "$out/display.log" 2>&1; then
    golden="$source_dir/Tests/UI/Golden/raspberry-pi-login.png"
    [ -z "${VULTURE_UPDATE_GOLDENS:-}" ] || cp "$out/login.png" "$golden"
    if "$build_dir/host/Tools/imgdiff/imgdiff" --output "$out/login-difference.png" "$golden" "$out/login.png"; then
        ok "the login window, on the firmware's framebuffer"
    else
        failed "the login window looks different; see $out/login-difference.png"
    fi
else
    failed "the login window never came; see $out/display.log"
fi

# ---- The SD card: what's written is there on the next boot, and the disk is sound.
card="$out/card.img"
"$build_dir/host/Tools/vfs/mkvfs" create "$card" 1024 "$build_dir/esp/System/Library/Kernels/vulture.bootarchive" > /dev/null
if run --boot-args utest=sd-card --disk "$card" --expect "Root: vultureFS on disk0s1" --expect "sd-card: wrote it" --expect "userland: PASS" --wait-for-exit --timeout 120 > "$out/card-write.log" 2>&1; then
    ok "writing to the SD card, ending by the watchdog"
else
    failed "writing to the SD card; see $out/card-write.log"
fi
if "$build_dir/host/Tools/vfs/fsck.vfs" "$card" > "$out/card-fsck.log" 2>&1; then
    ok "what it wrote is sound, by fsck"
else
    failed "fsck of the SD card; see $out/card-fsck.log"
fi
if run --boot-args utest=sd-card --disk "$card" --expect "sd-card: read back" --expect "userland: PASS" --wait-for-exit --timeout 120 > "$out/card-read.log" 2>&1; then
    ok "and it's still there after a restart"
else
    failed "what was written to the SD card is gone; see $out/card-read.log"
fi

# ---- A whole disk, as Meta/make-raspberry-pi-image.sh makes one (but with no firmware to put on it): its second
# ---- partition becomes the root, as the first isn't for the kernel.
empty_firmware="$out/no-firmware"
mkdir -p "$empty_firmware"
whole="$out/whole.img"
if "$source_dir/Meta/make-raspberry-pi-image.sh" --build-dir "$build_dir" --firmware "$empty_firmware" --size 1024 -o "$whole" > "$out/make-image.log" 2>&1; then
    if run --gui --image "$whole" --expect "Root: vultureFS" --expect "loginwindow: ready" --timeout 120 > "$out/whole.log" 2>&1; then
        ok "a whole disk, made for a Pi"
    else
        failed "the whole disk doesn't boot; see $out/whole.log"
    fi
    # ---- All the programs' tests, on the processor a Pi 4 has, and a console through its serial port: which, as the
    # ---- one device here with an interrupt in use, shows those come through.
    if run --boot-args utest --image "$whole" --expect "userland: PASS" --wait-for-exit --timeout 120 > "$out/userland.log" 2>&1; then
        ok "the tests of everything in userland"
    else
        failed "the tests of userland; see $out/userland.log"
        grep -a "CHECK\|\[FAIL\]\|^FAIL\|^userland:" "$out/userland.log" | head -20 >&2
    fi
    if run --chat "$source_dir/Tests/Console/login.chat" --fresh-disk --wait-for-exit --timeout 120 > "$out/console.log" 2>&1; then
        ok "logging in at the serial console, to power off from there"
    else
        failed "the serial console; see $out/console.log"
    fi
else
    failed "making a whole disk; see $out/make-image.log"
fi

if [ "$status" -eq 0 ]; then
    echo "raspberry-pi-test: PASS"
else
    echo "raspberry-pi-test: FAIL" >&2
fi
exit "$status"
