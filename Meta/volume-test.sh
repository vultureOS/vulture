#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# What a data volume does that takes the disk from outside to see: going back to a snapshot across a reboot, hearing
# about changes from before it, a flipped bit being reported, an update leaving the data alone, cloning 1 GiB.
#
#   Meta/volume-test.sh [--build-dir DIR]

set -uo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: volume-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

out="$build_dir/volume-test"
mkdir -p "$out"
# (Made anew each time, by mkvfs, and emptied again once it's done with: these get big.)
disk="$out/disk.img"
status=0
mkvfs="$build_dir/host/Tools/vfs/mkvfs"
fsck="$build_dir/host/Tools/vfs/fsck.vfs"
image="$build_dir/esp/System/Library/Kernels/vulture.bootarchive"

ok() { echo "volume-test: ok: $*"; }
failed() { echo "volume-test: FAIL: $*" >&2; status=1; }

boot() { # what to expect
    "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --disk "$disk" --boot-args "utest=volume-boots" --wait-for-exit --timeout 120 \
        --expect "userland: PASS" --expect "$1" > "$out/boot.log" 2>&1 < /dev/null
}

"$mkvfs" create "$disk" 2048 "$image" > /dev/null
touch "$disk.updated"

if boot "volume-boots: asked to go back"; then ok "asked to go back to a snapshot"; else failed "asking to go back to a snapshot"; tail -20 "$out/boot.log" >&2; fi
if boot "volume-boots: heard what happened before the machine started"; then
    ok "a snapshot, gone back to across a reboot, is exactly what it was; and it remembers what changed before then"
else
    failed "going back to a snapshot, or what's remembered"
    grep -a "volume-boots\|CHECK" "$out/boot.log" | tail >&2
fi
if "$fsck" -q "$disk" > "$out/fsck.log" 2>&1; then ok "fsck says that's all sound"; else failed "fsck after going back; see $out/fsck.log"; fi

# An update puts a new system in place, and it's the same data after it.
"$mkvfs" update "$disk" "$image" > /dev/null
if boot "volume-boots: wrote what will be damaged"; then ok "a system update leaves the data alone"; else failed "the data after an update"; fi

# A bit flipped where it shouldn't be.
python3 -I "$source_dir/Meta/flip-block.py" "$disk" text "volume-boots: damage me " > "$out/flip.log"
cat "$out/flip.log"
if "$fsck" -q "$disk" > "$out/fsck-damaged.log" 2>&1; then
    failed "fsck didn't notice a flipped bit"
else
    ok "fsck notices a flipped bit: $(grep -a 'fsck.vfs' "$out/fsck-damaged.log" | head -n 1 | cut -c1-120)"
fi
if boot "volume-boots: a damaged block says so" && grep -aq "vultureFS: inode .* is damaged" "$out/boot.log"; then
    ok "reading a flipped bit is an error, and it's logged, and the system goes on"
else
    failed "reading a flipped bit"
    grep -a "volume-boots\|CHECK\|damaged" "$out/boot.log" | tail >&2
fi
if "$fsck" -q "$disk" > "$out/fsck-after.log" 2>&1; then ok "with that file gone, fsck says it's all sound"; else failed "fsck after removing what was damaged; see $out/fsck-after.log"; fi

# A gibibyte, cloned: on a disk just for this, which only takes up what's written to it.
"$mkvfs" create "$disk" 3072 "$image" > /dev/null
touch "$disk.updated"
chat="$out/clone.chat"
cat > "$chat" <<'CHAT'
expect login:
send vulture\r
expect Password:
send vulture\r
expect vulture@vulture ~ %
send sudo /usr/tests/data-volume clone-a-gibibyte; echo clone-said=$?\r
expect Password:
send vulture\r
timeout 300
expect clone-said=
CHAT
if "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --disk "$disk" --chat "$chat" --timeout 400 > "$out/clone.log" 2>&1 < /dev/null; then
    result="$(grep -a 'clone-said=[0-9]' "$out/clone.log" | tail -n 1 | tr -d '\r')"
    took="$(grep -a 'cloned 1024 MiB' "$out/clone.log" | tail -n 1 | tr -d '\r')"
    if [ "${result##*=}" = 0 ]; then ok "$took, under 50 ms, and it shares its blocks"; else failed "cloning 1 GiB: $took"; fi
else
    failed "cloning 1 GiB"
    tail -20 "$out/clone.log" >&2
fi
: > "$disk"

if [ "$status" -eq 0 ]; then
    echo "volume-test: PASS"
else
    echo "volume-test: FAIL"
fi
exit "$status"
