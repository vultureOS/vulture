#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Does the system really make sound? Boots with QEMU recording what the sound device plays,
# has a program send a tone through the sound server, and looks for that tone in the
# recording: every layer, from the program to the device, has to have worked.
#
#   Meta/sound-test.sh [--build-dir DIR]

set -euo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: sound-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

qemu="${QEMU:-qemu-system-aarch64}"
if ! "$qemu" -device help 2>/dev/null | grep -q virtio-sound-pci; then
    echo "sound-test: this QEMU has no virtio-sound device: skipped"
    exit 0
fi

wavcheck="$build_dir/host/Tools/wavcheck/wavcheck"
out_dir="$build_dir/sound-test"
mkdir -p "$out_dir"
wav="$out_dir/heard.wav"
rm -f "$wav"

VULTURE_AUDIO_WAV="$wav" "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless \
    --boot-args soundtest \
    --expect "Audio: virtio-sound" \
    --expect "sound: played" \
    --wait-for-exit --timeout 60

"$wavcheck" "$wav" --tone 440 --seconds 0.9
echo "sound-test: PASS"
