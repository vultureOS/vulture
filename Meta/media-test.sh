#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Disks of the kinds other machines use: the same volumes looked at by this system's code and by a real Mac's own
# tools, in both directions; and a USB stick plugged into a running system, written to, ejected, and pulled out.
#
#   Meta/media-test.sh [--build-dir DIR]

set -uo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: media-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

out="$build_dir/media-test"
mkdir -p "$out"
exercise="$build_dir/host/Tests/LibForeignFS/foreignfs-exercise"
image_file="$build_dir/esp/System/Library/Kernels/vulture.bootarchive"
status=0

ok() { echo "media-test: ok: $*"; }
failed() { echo "media-test: FAIL: $*" >&2; status=1; }

on_mac=0
[ "$(uname -s)" = Darwin ] && on_mac=1

# ---- What a Mac makes of what's made here. (Its fsck wants a device, and an image can be one for a moment.)
mac_fsck() { # image kind
    local image="$1" kind="$2" device result
    device="$(hdiutil attach -nomount -noverify -imagekey diskimage-class=CRawDiskImage "$image" 2> "$out/attach.err" | head -n 1 | awk '{print $1}')"
    if [ -z "$device" ]; then
        cat "$out/attach.err" >&2
        return 2
    fi
    if [ "$kind" = exfat ]; then
        fsck_exfat -n "${device/\/dev\/disk//dev/rdisk}" > "$out/fsck.log" 2>&1
    else
        fsck_msdos -n "${device/\/dev\/disk//dev/rdisk}" > "$out/fsck.log" 2>&1
    fi
    result=$?
    hdiutil detach -quiet "$device" 2> /dev/null
    return "$result"
}

for kind in fat12 fat16 fat32 exfat; do
    size=64
    [ "$kind" = fat12 ] && size=2
    image="$out/$kind.img"
    if ! "$exercise" make "$image" "$size" "$kind" "VULTURE" > /dev/null; then
        failed "making a $kind volume"
        continue
    fi
    "$exercise" list "$image" > "$out/$kind.list" 2>&1
    if ! grep -aq "A rather long name for a file, with spaces.text" "$out/$kind.list" || ! grep -aq "café ☕ 😀.txt" "$out/$kind.list"; then
        failed "$kind: the names didn't come back"
    fi
    if [ "$on_mac" -eq 1 ]; then
        if mac_fsck "$image" "$kind"; then
            ok "$kind made here passes this Mac's fsck"
        else
            failed "$kind made here: this Mac's fsck says:"
            sed 's/^/    /' "$out/fsck.log" | head -n 30 >&2
        fi
    fi
    : > "$image"
done

if [ "$on_mac" -eq 1 ]; then
    # ---- What's made here of what a Mac makes, and the same again once that's been written to.
    tree="$out/mac-tree"
    /bin/mkdir -p "$tree/A Folder/Deeper"
    printf 'hello from a Mac\n' > "$tree/A Folder/Deeper/from a mac.txt"
    printf 'short\n' > "$tree/SHORT.TXT"
    printf 'not ascii\n' > "$tree/café ☕.txt"
    python3 -I -c 'import sys; open(sys.argv[1],"wb").write(bytes(range(256))*4096)' "$tree/large.bin"
    for kind in msdos exfat; do
        image="$out/mac-$kind.dmg"
        if [ "$kind" = exfat ]; then fs=ExFAT; else fs="MS-DOS FAT32"; fi
        if ! hdiutil create -quiet -ov -size 64m -fs "$fs" -layout NONE -volname MAC "$image"; then
            failed "this Mac making a $kind image"
            continue
        fi
        mount_point="$(hdiutil attach -nobrowse "$image" | tail -n 1 | sed 's/^.*\/Volumes/\/Volumes/')"
        /bin/cp -R "$tree/" "$mount_point/"
        hdiutil detach -quiet "$mount_point"
        "$exercise" list "$image" > "$out/mac-$kind.list" 2>&1
        if grep -aq "A Folder/Deeper/from a mac.txt 17" "$out/mac-$kind.list" && grep -aq "large.bin 1048576" "$out/mac-$kind.list" && grep -aq "café ☕.txt" "$out/mac-$kind.list"; then
            ok "a $kind volume made by this Mac reads here"
        else
            failed "a $kind volume made by this Mac: $(head -n 8 "$out/mac-$kind.list" | tr '\n' ' ')"
        fi
        if "$exercise" add "$image" > "$out/add.log" 2>&1; then
            if mac_fsck "$image" "$kind"; then
                ok "and with something added here, it still passes this Mac's fsck"
            else
                failed "added to a $kind volume here: this Mac's fsck says:"
                sed 's/^/    /' "$out/fsck.log" | head -n 30 >&2
            fi
            mount_point="$(hdiutil attach -nobrowse -readonly "$image" | tail -n 1 | sed 's/^.*\/Volumes/\/Volumes/')"
            if [ "$(/bin/cat "$mount_point/Added here/written by vultureOS.txt" 2>/dev/null)" = "Hello from over here." ]; then
                ok "and this Mac reads what was written"
            else
                failed "this Mac couldn't read what was added to a $kind volume"
            fi
            hdiutil detach -quiet "$mount_point"
        else
            failed "adding to a $kind volume made by this Mac: $(cat "$out/add.log")"
        fi
        : > "$image"
    done

    # ---- A disc.
    iso="$out/mac.iso"
    if hdiutil makehybrid -quiet -ov -iso -joliet -default-volume-name MACDISC -o "$iso" "$tree"; then
        "$exercise" list "$iso" > "$out/iso.list" 2>&1
        if grep -aq "A Folder/Deeper/from a mac.txt 17" "$out/iso.list" && grep -aq "type: cd9660" "$out/iso.list" && grep -aq "large.bin 1048576" "$out/iso.list"; then
            ok "an ISO 9660 disc made by this Mac reads here, by its long names"
        else
            failed "an ISO 9660 disc made by this Mac: $(head -n 8 "$out/iso.list" | tr '\n' ' ')"
        fi
    else
        failed "this Mac making a disc"
    fi
    : > "$iso"
fi

# ---- A USB stick, in a running system.
stick="$out/stick.img"
"$exercise" make "$stick" 64 exfat STICK > /dev/null
chat="$out/stick.chat"
cat > "$chat" <<CHAT
expect login:
send vulture\r
expect Password:
send vulture\r
expect vulture@vulture ~ %
send ls /Volumes; echo before-said=\$?\r
expect before-said=0
plug stick usb-storage bus=usb0.0,drive=stick
expect at /Volumes/STICK
send ls /Volumes/STICK | wc -l; echo listed-said=\$?\r
expect listed-said=0
send diskutil list | grep -c 'ExFAT STICK'; echo diskutil-said=\$?\r
expect diskutil-said=0
send cat "/Volumes/STICK/README.TXT"; echo read-said=\$?\r
expect A short name, in capitals.
expect read-said=0
send mkdir "/Volumes/STICK/From vultureOS" && cp /etc/hosts "/Volumes/STICK/From vultureOS/hosts copy.txt"; echo wrote-said=\$?\r
expect wrote-said=0
send cmp /etc/hosts "/Volumes/STICK/From vultureOS/hosts copy.txt"; echo same-said=\$?\r
expect same-said=0
send diskutil verifyVolume /Volumes/STICK; echo verify-said=\$?\r
expect Finished file system verification
expect verify-said=0
send diskutil eject /Volumes/STICK; echo eject-said=\$?\r
expect eject-said=0
send ls /Volumes/STICK; echo gone-said=\$?\r
expect gone-said=1
CHAT
if "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --fresh-disk --chat "$chat" --timeout 120 -- \
    -device qemu-xhci,id=usb0 -drive "if=none,id=stick,format=raw,file=$stick" > "$out/stick.log" 2>&1 < /dev/null; then
    ok "a USB stick plugged in comes up under /Volumes, reads, writes, and ejects"
else
    failed "the USB stick, plugged in: see $out/stick.log"
    tail -n 12 "$out/stick.log" | cut -c1-160 >&2
fi
"$exercise" list "$stick" > "$out/stick.list" 2>&1
if grep -aq "From vultureOS/hosts copy.txt" "$out/stick.list"; then
    ok "what was written is on it, seen from outside"
else
    failed "what was written isn't on the stick: $(head -n 6 "$out/stick.list" | tr '\n' ' ')"
fi
if [ "$on_mac" -eq 1 ]; then
    if mac_fsck "$stick" exfat; then
        ok "and it passes this Mac's fsck_exfat"
    else
        failed "the stick, after being written to: this Mac's fsck says:"
        sed 's/^/    /' "$out/fsck.log" | head -n 30 >&2
    fi
fi

# Damaged, as a machine losing power part way through might leave it: found, and put right.
for kind in fat32 exfat; do
    "$exercise" make "$stick" 64 "$kind" STICK > /dev/null
    python3 -I "$source_dir/Meta/damage-volume.py" "$stick"
    if [ "$on_mac" -eq 1 ] && mac_fsck "$stick" "$kind"; then
        failed "$kind: this Mac's fsck didn't notice the damage, so it's no test"
    fi
    chat="$out/repair.chat"
    cat > "$chat" <<CHAT
expect login:
send vulture\r
expect Password:
send vulture\r
expect vulture@vulture ~ %
plug stick usb-storage bus=usb0.0,drive=stick
expect at /Volumes/STICK
send diskutil verifyVolume /Volumes/STICK; echo verify-said=\$?\r
expect clusters are in use
expect verify-said=1
send diskutil repairVolume /Volumes/STICK; echo repair-said=\$?\r
expect Finished file system repair
expect repair-said=0
send diskutil verifyVolume /Volumes/STICK; echo again-said=\$?\r
expect again-said=0
send diskutil eject /Volumes/STICK; echo eject-said=\$?\r
expect eject-said=0
CHAT
    if "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --fresh-disk --chat "$chat" --timeout 120 -- \
        -device qemu-xhci,id=usb0 -drive "if=none,id=stick,format=raw,file=$stick" > "$out/repair.log" 2>&1 < /dev/null; then
        ok "$kind: damage is found, and put right"
    else
        failed "$kind: repairing: see $out/repair.log"
        tail -n 12 "$out/repair.log" | cut -c1-160 >&2
    fi
    if [ "$on_mac" -eq 1 ]; then
        if mac_fsck "$stick" "$kind"; then
            ok "$kind: and what's left passes this Mac's fsck"
        else
            failed "$kind: after repairing: this Mac's fsck says:"
            sed 's/^/    /' "$out/fsck.log" | head -n 30 >&2
        fi
    fi
    "$exercise" list "$stick" > "$out/repaired.list" 2>&1
    grep -aq "A Folder/inside.txt" "$out/repaired.list" && ok "$kind: and nothing on it was lost" || failed "$kind: a repair lost what was on it"
done

# Erased, and divided, and made into a disk with what's on an image of its own.
"$exercise" make "$stick" 64 exfat STICK > /dev/null
chat="$out/erase.chat"
cat > "$chat" <<CHAT
expect login:
send vulture\r
expect Password:
send vulture\r
expect vulture@vulture ~ %
plug stick usb-storage bus=usb0.0,drive=stick
expect at /Volumes/STICK
send sudo diskutil eraseDisk ExFAT WHOLE GPT /Volumes/STICK; echo erase-said=\$?\r
expect Password:
send vulture\r
expect at /Volumes/WHOLE
expect erase-said=0
send diskutil partitionDisk /Volumes/WHOLE 2 GPT ExFAT ONE 50 MS-DOS TWO R; echo partition-said=\$?\r
expect at /Volumes/TWO
expect partition-said=0
send echo first > /Volumes/ONE/file; echo second > /Volumes/TWO/file; grep . /Volumes/ONE/file /Volumes/TWO/file; echo wrote-said=\$?\r
expect /Volumes/TWO/file:second
expect wrote-said=0
send diskutil eject /Volumes/ONE; echo eject-said=\$?\r
expect eject-said=0
send cd /tmp; hdiutil create -size 16m -fs ExFAT -volname IMAGE made; echo create-said=\$?\r
expect created: made.dmg
expect create-said=0
send hdiutil attach made.dmg; echo attach-said=\$?\r
expect at /Volumes/IMAGE
expect attach-said=0
send echo inside > /Volumes/IMAGE/note; hdiutil detach /Volumes/IMAGE; echo detach-said=\$?\r
expect detach-said=0
send ls /Volumes/IMAGE; echo gone-said=\$?\r
expect gone-said=1
send hdiutil attach -readonly made.dmg > /dev/null; grep . /Volumes/IMAGE/note; echo reattach-said=\$?\r
expect inside
expect reattach-said=0
send echo no > /Volumes/IMAGE/other; echo readonly-said=\$?\r
expect readonly-said=1
send hdiutil detach /Volumes/IMAGE; echo end-said=\$?\r
expect end-said=0
CHAT
if "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --fresh-disk --chat "$chat" --timeout 180 -- \
    -device qemu-xhci,id=usb0 -drive "if=none,id=stick,format=raw,file=$stick" > "$out/erase.log" 2>&1 < /dev/null; then
    ok "a stick erased and divided, and a disk image made, attached, written and read"
else
    failed "erasing, partitioning and images: see $out/erase.log"
    tail -n 14 "$out/erase.log" | cut -c1-160 >&2
fi
if [ "$on_mac" -eq 1 ]; then
    device="$(hdiutil attach -nomount -noverify -imagekey diskimage-class=CRawDiskImage "$stick" 2> /dev/null | head -n 1 | awk '{print $1}')"
    if [ -n "$device" ]; then
        if fsck_exfat -n "${device/\/dev\/disk//dev/rdisk}s1" > "$out/fsck.log" 2>&1 && fsck_msdos -n "${device/\/dev\/disk//dev/rdisk}s2" >> "$out/fsck.log" 2>&1; then
            ok "and both of what it was divided into pass this Mac's fsck"
        else
            failed "a stick, divided: this Mac's fsck says:"
            sed 's/^/    /' "$out/fsck.log" | head -n 30 >&2
        fi
        hdiutil detach -quiet "$device" 2> /dev/null
    else
        failed "this Mac couldn't attach the divided stick"
    fi
fi

# Two systems on one disk: which it starts from is whichever was chosen.
architecture="$(cat "$build_dir/root/System/Library/CoreServices/Architecture" 2> /dev/null || echo aarch64)"
boot_file=BOOTAA64.EFI
[ "$architecture" = x86_64 ] && boot_file=BOOTX64.EFI
esp="$out/esp"
mkdir -p "$esp/EFI/BOOT" "$esp/System/Library/Kernels"
cp "$build_dir/esp/EFI/BOOT/$boot_file" "$esp/EFI/BOOT/"
cp "$build_dir/esp/System/Library/Kernels/vulture" "$image_file" "$esp/System/Library/Kernels/"
printf 'console\n' > "$esp/System/Library/Kernels/boot-args"
two="$out/two.img"
"$build_dir/host/Tools/vfs/mkimage" "$two" 2048 "$esp" "$image_file+$image_file" \
    "EFI/BOOT/$boot_file" System/Library/Kernels/vulture System/Library/Kernels/vulture.bootarchive System/Library/Kernels/boot-args > /dev/null
chat="$out/startup.chat"
cat > "$chat" <<CHAT
expect login:
send vulture\r
expect Password:
send vulture\r
expect vulture@vulture ~ %
send bless --getBoot; diskutil list | grep -c 'vultureFS System'; echo list-said=\$?\r
expect list-said=0
send sudo bless --device /dev/disk0s4 --setBoot; echo bless-said=\$?\r
expect Password:
send vulture\r
expect bless-said=0
CHAT
if "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --image "$two" --chat "$chat" --timeout 120 > "$out/startup1.log" 2>&1 < /dev/null \
    && grep -aq "Root: vultureFS on disk0s2" "$out/startup1.log" \
    && "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --image "$two" --expect "login:" --timeout 60 > "$out/startup2.log" 2>&1 < /dev/null \
    && grep -aq "Root: vultureFS on disk0s4" "$out/startup2.log"; then
    ok "a machine with two systems starts from whichever was chosen"
else
    failed "choosing a startup disk: see $out/startup1.log and $out/startup2.log"
    grep -a "Root:\|bless\|chat:" "$out/startup1.log" "$out/startup2.log" | cut -c1-160 >&2
fi
: > "$two"

# And taken out with no warning, with something on it in use. The system goes on.
"$exercise" make "$stick" 64 fat32 STICK > /dev/null
chat="$out/pulled.chat"
cat > "$chat" <<CHAT
expect login:
send vulture\r
expect Password:
send vulture\r
expect vulture@vulture ~ %
plug stick usb-storage bus=usb0.0,drive=stick
expect at /Volumes/STICK
send cat > /Volumes/STICK/being-written &\r
expect vulture@vulture ~ %
send cd /Volumes/STICK; pwd\r
expect /Volumes/STICK
unplug stick
expect was taken away
send cd; ls /Volumes/STICK; echo after-said=\$?\r
expect after-said=1
send echo still-here; uname -s\r
expect still-here
expect vultureOS
forbid KERNEL PANIC
CHAT
if "$source_dir/Meta/run.sh" --build-dir "$build_dir" --headless --fresh-disk --chat "$chat" --timeout 120 -- \
    -device qemu-xhci,id=usb0 -drive "if=none,id=stick,format=raw,file=$stick" > "$out/pulled.log" 2>&1 < /dev/null; then
    ok "pulled out with no warning, in use: the system goes on, and no panic"
else
    failed "the USB stick, pulled out: see $out/pulled.log"
    tail -n 12 "$out/pulled.log" | cut -c1-160 >&2
fi
: > "$stick"

if [ "$status" -eq 0 ]; then
    echo "media-test: PASS"
else
    echo "media-test: FAIL"
fi
exit "$status"
