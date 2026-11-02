#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The drivers for what real machines have, on QEMU's models of them: an AHCI root disk, xHCI with a hub,
# a keyboard behind the hub and a USB stick, a tablet, e1000e and Intel HD Audio. Whichever architecture
# the build directory is for; a real machine is another matter, which this says nothing about.
#
#   Meta/hardware-test.sh [--build-dir DIR]

set -uo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: hardware-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

build_dir="$(cd "$build_dir" && pwd)"
out="$build_dir/hardware-test"
mkdir -p "$out"
status=0

# ---- A USB stick, behind a hub, with something on it to recognize.
stick="$out/stick.img"
python3 -I - "$stick" <<'PYTHON'
import sys
with open(sys.argv[1], "wb") as stick:
    first = b"vultureOS USB test stick\0"
    stick.write(first + bytes(512 - len(first)))
    stick.truncate(16 * 1024 * 1024)
PYTHON

output="$("$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --real-devices --fresh-disk --boot-args utest=usb-disk \
    --expect "usb-disk: read back what was written" --expect "userland: PASS" --wait-for-exit --timeout 90 -- \
    -drive "if=none,id=stick,format=raw,file=$stick" \
    -device usb-hub,bus=usb0.0,port=3 \
    -device usb-storage,bus=usb0.0,port=3.1,drive=stick 2>&1)"
if [ $? -eq 0 ]; then
    echo "hardware-test: ok: a USB stick behind a hub: read and written"
else
    echo "hardware-test: FAIL: the USB stick" >&2
    printf '%s\n' "$output" | grep -a "USB\|xHCI\|usb-disk\|CHECK\|KERNEL PANIC\|Storage:" | head -20 >&2
    status=1
fi

# And it was really written, as seen from outside.
python3 -I - "$stick" <<'PYTHON' || status=1
import sys
with open(sys.argv[1], "rb") as stick:
    stick.seek(8 * 512)
    data = stick.read(400 * 512)
wrong = sum(1 for i, byte in enumerate(data) if byte != (i * 31 + i // 512) & 0xff)
if wrong:
    print(f"hardware-test: FAIL: {wrong} bytes on the stick are wrong", file=sys.stderr)
    sys.exit(1)
print("hardware-test: ok: what's on the stick is what was written")
PYTHON

# ---- The disk, through a power cut's worth of boots.
if VULTURE_REAL_DEVICES=1 "$source_dir/Meta/storage-test.sh" --build-dir "$build_dir" persistence > "$out/persistence.log" 2>&1; then
    echo "hardware-test: ok: three boots on one AHCI disk"
else
    echo "hardware-test: FAIL: AHCI persistence; see $out/persistence.log" >&2
    status=1
fi

# ---- Sound, recorded and looked for.
if VULTURE_REAL_DEVICES=1 "$source_dir/Meta/sound-test.sh" --build-dir "$build_dir" > "$out/sound.log" 2>&1; then
    echo "hardware-test: ok: a tone through Intel HD Audio"
else
    echo "hardware-test: FAIL: Intel HD Audio; see $out/sound.log" >&2
    status=1
fi

# ---- The network and the keyboard and pointer, all the way through Software's window.
# (On a screen of the size what it clicks on was measured for.)
if VULTURE_REAL_DEVICES=1 VULTURE_KEEP_VIRTIO_DISPLAY=1 "$source_dir/Meta/network-test.sh" --build-dir "$build_dir" > "$out/network.log" 2>&1; then
    echo "hardware-test: ok: e1000e, and USB input, through Software"
else
    echo "hardware-test: FAIL: e1000e or USB input; see $out/network.log" >&2
    status=1
fi

# ---- With nothing but the firmware's framebuffer to draw on, and the pointer drawn by the driver.
imgdiff="$build_dir/host/Tools/imgdiff/imgdiff"
# (What the firmware offers differs between architectures, so this does too.)
architecture="$(sed -n 's/^VULTURE_ARCH:[A-Z]*=//p' "$build_dir/CMakeCache.txt" 2>/dev/null || true)"
golden="$source_dir/Tests/UI/Golden/framebuffer-${architecture:-aarch64}-terminal.png"
if VULTURE_REAL_DEVICES=1 VULTURE_SCREENSHOT_PREFIX="$out/" "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --gui --fresh-disk \
    --boot-args uidesktop --chat "$source_dir/Tests/UI/framebuffer.chat" --timeout 60 > "$out/framebuffer.log" 2>&1; then
    for _ in $(seq 1 50); do
        [ -s "$out/terminal.png" ] && break
        sleep 0.1
    done
    if [ "${VULTURE_UPDATE_GOLDENS:-0}" = 1 ]; then
        cp "$out/terminal.png" "$golden"
        echo "hardware-test: updated $golden"
    elif "$imgdiff" --output "$out/terminal-difference.png" "$golden" "$out/terminal.png"; then
        echo "hardware-test: ok: the desktop on the firmware's framebuffer"
    else
        echo "hardware-test: FAIL: the firmware's framebuffer differs from the golden; see $out/terminal-difference.png" >&2
        status=1
    fi
else
    echo "hardware-test: FAIL: the firmware's framebuffer; see $out/framebuffer.log" >&2
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "hardware-test: PASS"
else
    echo "hardware-test: FAIL" >&2
fi
exit "$status"
