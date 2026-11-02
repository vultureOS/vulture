#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A disk for a Raspberry Pi 4 or 5, to write to an SD card: what its firmware loads on the first partition, and
# vultureFS on the rest. The firmware itself comes from you (it's the Raspberry Pi Foundation's, under its own
# license, so it isn't in this repository): see Documentation/RaspberryPi.md.
#
#   Meta/make-raspberry-pi-image.sh [--build-dir DIR] --firmware DIR [--size MIB] -o OUTPUT
#
# (QEMU takes only sizes that are a power of 2 for an SD card. A real card doesn't care.)

set -euo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
firmware=""
size=4096
output=""
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --firmware) firmware="$2"; shift 2 ;;
        --size) size="$2"; shift 2 ;;
        -o) output="$2"; shift 2 ;;
        *) echo "usage: make-raspberry-pi-image.sh [--build-dir DIR] --firmware DIR [--size MIB] -o OUTPUT" >&2; exit 2 ;;
    esac
done
[ -n "$output" ] || { echo "make-raspberry-pi-image.sh: where to? (-o OUTPUT)" >&2; exit 2; }
[ -n "$firmware" ] || { echo "make-raspberry-pi-image.sh: where's the firmware? (--firmware DIR; see Documentation/RaspberryPi.md)" >&2; exit 2; }

# shellcheck source=raspberry-pi-files.sh
source "$source_dir/Meta/raspberry-pi-files.sh"

esp="$(dirname "$output")/raspberry-pi-esp"
rm -rf "$esp"
mkdir -p "$esp/System/Library/Kernels"
trap 'rm -rf "$esp"' EXIT
add_raspberry_pi_files "$esp" "$firmware" "$build_dir"
# The usual loader and kernel too: UEFI firmware for the Pi exists, and it's what the Installer takes the system from.
archive="$build_dir/esp/System/Library/Kernels/vulture.bootarchive"
mkdir -p "$esp/EFI/BOOT"
cp "$build_dir/esp/EFI/BOOT/BOOTAA64.EFI" "$esp/EFI/BOOT/"
cp "$build_dir/esp/System/Library/Kernels/vulture" "$archive" "$esp/System/Library/Kernels/"
"$build_dir/host/Tools/vfs/mkimage" "$output" "$size" "$esp" "$archive" \
    "${raspberry_pi_files[@]}" EFI/BOOT/BOOTAA64.EFI System/Library/Kernels/vulture System/Library/Kernels/vulture.bootarchive
