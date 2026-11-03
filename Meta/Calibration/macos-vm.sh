#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A macOS 15 virtual machine, for measuring what Sequoia really looks like: see README.md. This only builds the small program
# that runs it (vm/main.swift, on Apple's own Virtualization framework: there's nothing else to install) and hands over to it.
#
#   Meta/Calibration/macos-vm.sh check
#   Meta/Calibration/macos-vm.sh install --ipsw FILE [--directory DIR] [--disk-gb N]
#   Meta/Calibration/macos-vm.sh run [--directory DIR]

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
tool="${TMPDIR:-/tmp}/vulture-macos-vm"
if [ ! -x "$tool" ] || [ "$here/vm/main.swift" -nt "$tool" ]; then
    /usr/bin/swiftc -O -framework Virtualization -framework AppKit "$here/vm/main.swift" -o "$tool"
    /usr/bin/codesign --force --sign - --entitlements "$here/vm/entitlements.plist" "$tool"
fi
# What's to be run inside it, once, where it can get at it.
if [ "${1:-}" = run ]; then
    directory="${VULTURE_CALIBRATION_VM:-$HOME/vulture-calibration-vm}"
    previous=""
    for argument in "$@"; do
        [ "$previous" = --directory ] && directory="$argument"
        previous="$argument"
    done
    mkdir -p "$directory/share/calibration"
    cp "$here/guest-setup.sh" "$directory/share/calibration/"
fi
exec "$tool" "$@"
