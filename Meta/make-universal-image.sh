#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One disk image that boots both kinds of machine: the firmware of each finds its own loader on the EFI
# System Partition, the loader its own kernel, and the kernel its own vultureFS partition.
#
#   Meta/make-universal-image.sh [--aarch64 BUILD_DIR] [--x86_64 BUILD_DIR] [--size MIB] -o OUTPUT

set -euo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
aarch64="$source_dir/build"
x86_64="$source_dir/build-x86_64"
size=8192
output=""
while [ $# -gt 0 ]; do
    case "$1" in
        --aarch64) aarch64="$2"; shift 2 ;;
        --x86_64) x86_64="$2"; shift 2 ;;
        --size) size="$2"; shift 2 ;;
        -o) output="$2"; shift 2 ;;
        *) echo "usage: make-universal-image.sh [--aarch64 DIR] [--x86_64 DIR] [--size MIB] -o OUTPUT" >&2; exit 2 ;;
    esac
done
[ -n "$output" ] || { echo "make-universal-image.sh: where to? (-o OUTPUT)" >&2; exit 2; }

esp="$(dirname "$output")/universal-esp"
rm -rf "$esp"
mkdir -p "$esp/EFI/BOOT" "$esp/System/Library/Kernels"
cp "$aarch64/esp/EFI/BOOT/BOOTAA64.EFI" "$x86_64/esp/EFI/BOOT/BOOTX64.EFI" "$esp/EFI/BOOT/"
for pair in "aarch64:$aarch64" "x86_64:$x86_64"; do
    name="${pair%%:*}"
    dir="${pair#*:}"
    cp "$dir/esp/System/Library/Kernels/vulture" "$esp/System/Library/Kernels/vulture.$name"
    cp "$dir/esp/System/Library/Kernels/vulture.bootarchive" "$esp/System/Library/Kernels/vulture.bootarchive.$name"
done
"$aarch64/host/Tools/vfs/mkimage" "$output" "$size" "$esp" \
    "$aarch64/esp/System/Library/Kernels/vulture.bootarchive+$x86_64/esp/System/Library/Kernels/vulture.bootarchive" \
    EFI/BOOT/BOOTAA64.EFI EFI/BOOT/BOOTX64.EFI \
    System/Library/Kernels/vulture.aarch64 System/Library/Kernels/vulture.bootarchive.aarch64 \
    System/Library/Kernels/vulture.x86_64 System/Library/Kernels/vulture.bootarchive.x86_64
rm -rf "$esp"
