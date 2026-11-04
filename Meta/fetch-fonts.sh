#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The fonts for other languages and for emoji: Noto, under the SIL Open Font License. They're not in the repository: this fetches what Meta/fonts.lock says, checks each against its checksum,
# and leaves them where the build finds them. Without them, the system still works, and text in those languages shows as empty boxes.
#
#   Meta/fetch-fonts.sh [--build-dir DIR]

set -euo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: fetch-fonts.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

out="$build_dir/fonts"
mkdir -p "$out"
status=0
while read -r checksum name url; do
    case "$checksum" in
        \#*|"") continue ;;
    esac
    if [ -s "$out/$name" ] && [ "$(shasum -a 256 "$out/$name" | cut -d' ' -f1)" = "$checksum" ]; then
        continue
    fi
    echo "fetch-fonts: $name"
    curl -sS -f --retry 2 --max-time 300 -o "$out/$name.part" "$url"
    if [ "$(shasum -a 256 "$out/$name.part" | cut -d' ' -f1)" != "$checksum" ]; then
        echo "fetch-fonts: $name isn't what it's meant to be" >&2
        rm -f "$out/$name.part"
        status=1
        continue
    fi
    mv "$out/$name.part" "$out/$name"
done < "$source_dir/Meta/fonts.lock"
exit "$status"
