#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Boots vultureOS in QEMU's AArch64 "virt" machine with UEFI firmware.
#
#   Meta/run.sh [options] [-- extra QEMU arguments]
#
#   --build-dir DIR    build directory (default: <repo>/build)
#   --tcg              software emulation instead of Apple Hypervisor (HVF)
#   --gdb              wait for a debugger on localhost:1234
#   --headless         no window; the serial console is captured and printed
#   --expect TEXT      with --headless: succeed once TEXT appears on the console
#   --timeout SECONDS  with --headless: give up after SECONDS (default: 60)

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$root_dir/build"
accelerator="hvf"
gdb=0
headless=0
expect=""
timeout_seconds=60
extra_args=()

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --tcg) accelerator="tcg"; shift ;;
        --gdb) gdb=1; shift ;;
        --headless) headless=1; shift ;;
        --expect) expect="$2"; shift 2 ;;
        --timeout) timeout_seconds="$2"; shift 2 ;;
        --) shift; extra_args=("$@"); break ;;
        -h|--help) sed -n '8,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "run.sh: unknown option '$1' (see --help)" >&2; exit 2 ;;
    esac
done

qemu="${QEMU:-qemu-system-aarch64}"
firmware_dir="${VULTURE_FIRMWARE_DIR:-/opt/homebrew/share/qemu}"
esp_dir="$build_dir/esp"
state_dir="$build_dir/qemu"

if [ ! -f "$esp_dir/EFI/BOOT/BOOTAA64.EFI" ]; then
    echo "run.sh: no boot loader at $esp_dir/EFI/BOOT/BOOTAA64.EFI; build first with: ninja -C $build_dir" >&2
    exit 1
fi

# The firmware keeps its settings (boot order etc.) in a writable variable store.
mkdir -p "$state_dir"
if [ ! -f "$state_dir/efivars.fd" ]; then
    cp "$firmware_dir/edk2-arm-vars.fd" "$state_dir/efivars.fd"
fi

args=(
    # acpi=off makes the firmware publish a device tree, which the kernel uses for hardware discovery.
    -machine virt,gic-version=3,acpi=off
    -smp 8
    -m 4G
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$firmware_dir/edk2-aarch64-code.fd"
    -drive "if=pflash,format=raw,unit=1,file=$state_dir/efivars.fd"
    # The EFI System Partition is the staging directory, exposed as a read-only FAT disk.
    -drive "if=none,id=esp,format=raw,readonly=on,file=fat:$esp_dir"
    -device virtio-blk-pci,drive=esp,bootindex=0
    # ramfb gives the firmware a linear framebuffer for the boot console; virtio-gpu
    # only offers blit operations until vultureOS has its own driver (M8).
    -device ramfb
    -device qemu-xhci
    -device usb-kbd
    -device usb-tablet
    -nic none
)

if [ "$accelerator" = "hvf" ]; then
    args+=(-accel hvf -cpu host)
else
    args+=(-accel tcg,thread=multi -cpu max)
fi

if [ "$gdb" -eq 1 ]; then
    args+=(-s -S)
    echo "run.sh: waiting for a debugger on localhost:1234" >&2
fi

if [ "$headless" -eq 0 ]; then
    exec "$qemu" "${args[@]}" -serial stdio ${extra_args[@]+"${extra_args[@]}"}
fi

log="$state_dir/serial.log"
: > "$log"
"$qemu" "${args[@]}" -display none -monitor none -serial "file:$log" ${extra_args[@]+"${extra_args[@]}"} &
qemu_pid=$!
trap 'kill "$qemu_pid" 2>/dev/null || true' EXIT

found=0
deadline=$((SECONDS + timeout_seconds))
while kill -0 "$qemu_pid" 2>/dev/null; do
    if [ -n "$expect" ] && grep -qF -- "$expect" "$log"; then
        found=1
        break
    fi
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "run.sh: timed out after ${timeout_seconds}s" >&2
        break
    fi
    sleep 0.2
done

kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true
trap - EXIT

# QEMU may have exited on its own just after printing the marker.
if [ -n "$expect" ] && [ "$found" -eq 0 ] && grep -qF -- "$expect" "$log"; then
    found=1
fi

# Show the console without the firmware's terminal control sequences.
sed -e $'s/\x1b\\[[0-9;?=]*[A-Za-z]//g' -e $'s/\r//g' "$log"

if [ -z "$expect" ]; then
    exit 0
fi
if [ "$found" -eq 1 ]; then
    echo "run.sh: PASS (found \"$expect\")"
    exit 0
fi
echo "run.sh: FAIL (\"$expect\" not found)" >&2
exit 1
