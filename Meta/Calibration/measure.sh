#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds measure.m against a macOS 15 SDK and runs it: AppKit draws as that SDK's
# macOS does, on whatever newer macOS this is. Only on a Mac, and only for people
# calibrating the look; nothing in the OS or its build depends on it.
#
#   Meta/Calibration/measure.sh [--pictures DIR] > measured.json

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
sdk=""
for candidate in /Library/Developer/CommandLineTools/SDKs/MacOSX15*.sdk /Applications/Xcode*.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15*.sdk; do
    [ -d "$candidate" ] && sdk="$candidate"
done
if [ -z "$sdk" ]; then
    echo "measure: no macOS 15 SDK here: AppKit would draw today's look, not Sequoia's" >&2
    exit 1
fi

out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
# Apple's clang: other compilers cannot read these headers.
/usr/bin/clang -fobjc-arc -Wno-deprecated-declarations -isysroot "$sdk" -mmacosx-version-min=14.0 \
    -framework AppKit "$here/measure.m" -o "$out/measure"
# The default accent, whatever this Mac is set to.
"$out/measure" "$@" -AppleAccentColor 4 -AppleHighlightColor "0.698039 0.843137 1.000000 Blue"
