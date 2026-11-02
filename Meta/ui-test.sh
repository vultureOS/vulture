#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Golden screenshots: boots the scene in Tests/UI on a display of each size and
# compares what PranaWindowServer composed with Tests/UI/Golden, by imgdiff's
# rule (no channel off by more than 2 in more than 0.1% of pixels).
#
#   Meta/ui-test.sh [--build-dir DIR] [--update]
#
# --update makes the screenshots the new goldens: look at them first.

set -euo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
update=0
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --update) update=1; shift ;;
        *) echo "usage: ui-test.sh [--build-dir DIR] [--update]" >&2; exit 2 ;;
    esac
done

imgdiff="$build_dir/host/Tools/imgdiff/imgdiff"
out_dir="$build_dir/ui-test"
golden_dir="$source_dir/Tests/UI/Golden"
mkdir -p "$out_dir" "$golden_dir"

status=0
# The name, the display's size and what to do first: the same scene in points, at
# each scale, and once dragged by the real pointer (the tablet device, /dev/input,
# the window server).
for config in "scene-2x 2560 1600 still" "scene-1x 1280 800 still" "scene-dragged 2560 1600 drag"; do
    read -r name width height script <<< "$config"
    shot="$out_dir/$name.png"
    rm -f "$shot"
    if ! VULTURE_GPU_WIDTH="$width" VULTURE_GPU_HEIGHT="$height" VULTURE_SCREENSHOT_DEVICE=gpu0 \
        "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --boot-args uitest \
        --chat "$source_dir/Tests/UI/$script.chat" --screenshot "$shot" --timeout 90 > "$out_dir/$name.log" 2>&1; then
        echo "ui-test: $name: the scene did not come up; see $out_dir/$name.log" >&2
        status=1
        continue
    fi
    # The screenshot is written as QEMU gets around to it.
    for _ in $(seq 1 50); do
        [ -s "$shot" ] && break
        sleep 0.1
    done
    if [ "$update" -eq 1 ]; then
        cp "$shot" "$golden_dir/$name.png"
        echo "ui-test: $name: updated"
    elif ! "$imgdiff" --output "$out_dir/$name-difference.png" "$golden_dir/$name.png" "$shot"; then
        echo "ui-test: $name: differs from the golden; see $out_dir/$name-difference.png" >&2
        status=1
    fi
done

# The Widget Gallery on the real server: one boot walks through all of it, at each
# scale, in both appearances. And the desktop, from the login window on.
chat_shots() {
    grep '^screenshot ' "$source_dir/Tests/UI/$1.chat" | cut -d' ' -f2
}
for config in "gallery-2x 2560 1600 gallery uigallery" "gallery-1x 1280 800 gallery uigallery" "desktop-2x 2560 1600 desktop uidesktop" "apps-2x 2560 1600 apps uidesktop"; do
    read -r name width height chat flag <<< "$config"
    rm -f "$out_dir/$name"-*.png
    if ! VULTURE_GPU_WIDTH="$width" VULTURE_GPU_HEIGHT="$height" VULTURE_SCREENSHOT_DEVICE=gpu0 VULTURE_SCREENSHOT_PREFIX="$out_dir/$name-" \
        "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --fresh-disk --boot-args "$flag" \
        --chat "$source_dir/Tests/UI/$chat.chat" --timeout 240 > "$out_dir/$name.log" 2>&1; then
        echo "ui-test: $name: it did not get through; see $out_dir/$name.log" >&2
        status=1
        continue
    fi
    for shot in $(chat_shots "$chat"); do
        # (Numbers that change from run to run: to look at, not to compare.)
        case "$shot" in look-*) continue ;; esac
        # Written as QEMU gets around to it.
        for _ in $(seq 1 50); do
            [ -s "$out_dir/$name-$shot" ] && break
            sleep 0.1
        done
        if [ "$update" -eq 1 ]; then
            cp "$out_dir/$name-$shot" "$golden_dir/$name-$shot"
        elif ! "$imgdiff" --output "$out_dir/$name-$shot-difference.png" "$golden_dir/$name-$shot" "$out_dir/$name-$shot"; then
            echo "ui-test: $name-$shot: differs from the golden; see $out_dir/$name-$shot-difference.png" >&2
            status=1
        fi
    done
    [ "$update" -eq 1 ] && echo "ui-test: $name: updated"
done
# The pointer. It's the display's own to draw, so it isn't in any screenshot above. Its shapes are compared on paper...
sheet="$out_dir/cursors.png"
"$build_dir/host/Tests/WindowServer/cursor-sheet" "$sheet"
if [ "$update" -eq 1 ]; then
    cp "$sheet" "$golden_dir/cursors.png"
elif ! "$imgdiff" --output "$out_dir/cursors-difference.png" "$golden_dir/cursors.png" "$sheet"; then
    echo "ui-test: cursors.png: differs from the golden; see $out_dir/cursors-difference.png" >&2
    status=1
fi
# ...and that it's shown at all, by what QEMU is told. It reads a move with no resource as "there's no cursor now".
if "${QEMU:-qemu-system-aarch64}" -trace help 2>/dev/null | grep -q '^virtio_gpu_update_cursor$'; then
    trace="$out_dir/pointer-trace.log"
    rm -f "$trace"
    if VULTURE_GPU_WIDTH=1280 VULTURE_GPU_HEIGHT=800 "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --gui --fresh-disk \
        --chat "$source_dir/Tests/UI/pointer.chat" --timeout 60 -- -trace virtio_gpu_update_cursor -D "$trace" > "$out_dir/pointer.log" 2>&1; then
        moves="$(grep -a -c 'virtio_gpu_update_cursor.*move' "$trace" || true)"
        hidden="$(grep -a -c 'virtio_gpu_update_cursor.*move, res 0x0' "$trace" || true)"
        if [ "$moves" -lt 3 ] || [ "$hidden" -ne 0 ]; then
            echo "ui-test: the pointer: of $moves moves, $hidden would hide it; see $trace" >&2
            status=1
        fi
    else
        echo "ui-test: the pointer: it did not get through; see $out_dir/pointer.log" >&2
        status=1
    fi
fi

[ "$status" -eq 0 ] && echo "ui-test: PASS" || echo "ui-test: FAIL" >&2
exit "$status"
