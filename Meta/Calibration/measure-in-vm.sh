#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The same measuring program as measure.sh, but running on a real macOS 15, in the virtual machine, rather than linked against
# its SDK on whatever this Mac has: so that what's measured is Sequoia, including what AppKit doesn't decide by the SDK.
# Prints the measurements as JSON. Only numbers leave the virtual machine for the repository.
#
#   Meta/Calibration/measure-in-vm.sh [--directory DIR] [--pictures DIR] > measured.json

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
directory="${VULTURE_CALIBRATION_VM:-$HOME/vulture-calibration-vm}"
pictures=""
while [ $# -gt 0 ]; do
    case "$1" in
        --directory) directory="$2"; shift 2 ;;
        --pictures) pictures="$2"; shift 2 ;;
        *) echo "usage: measure-in-vm.sh [--directory DIR] [--pictures DIR]" >&2; exit 2 ;;
    esac
done

sdk=""
for candidate in /Library/Developer/CommandLineTools/SDKs/MacOSX15*.sdk /Applications/Xcode*.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15*.sdk; do
    [ -d "$candidate" ] && sdk="$candidate"
done
if [ -z "$sdk" ]; then
    echo "measure-in-vm: no macOS 15 SDK here to build the measuring program with" >&2
    exit 1
fi
share="$directory/share/calibration"
mkdir -p "$share"
/usr/bin/clang -fobjc-arc -Wno-deprecated-declarations -isysroot "$sdk" -mmacosx-version-min=14.0 -framework AppKit "$here/measure.m" -o "$share/measure"

version="$("$here/in-vm.sh" --directory "$directory" "sw_vers -productVersion")"
case "$version" in
    15.*) ;;
    *)
        echo "measure-in-vm: this virtual machine runs macOS $version: that's not Sequoia" >&2
        exit 1
        ;;
esac
echo "measure-in-vm: measuring on macOS $version" >&2

arguments='-AppleAccentColor 4 -AppleHighlightColor "0.698039 0.843137 1.000000 Blue"'
if [ -n "$pictures" ]; then
    mkdir -p "$share/pictures"
    arguments="--pictures pictures $arguments"
fi
"$here/in-vm.sh" --directory "$directory" "./measure $arguments"
if [ -n "$pictures" ]; then
    mkdir -p "$pictures"
    cp "$share"/pictures/* "$pictures"/
fi
