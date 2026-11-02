#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by the scripts that make disks: puts what a Raspberry Pi starts from on an EFI System Partition being
# put together, and lists it, relative to that, in raspberry_pi_files.
#
#   add_raspberry_pi_files ESP_DIRECTORY FIRMWARE_DIRECTORY AARCH64_BUILD_DIRECTORY
#
# The firmware is the Raspberry Pi Foundation's, and isn't part of vultureOS: see Documentation/RaspberryPi.md.

raspberry_pi_files=()

add_raspberry_pi_files() {
    local esp="$1" firmware="$2" build="$3"
    if [ ! -d "$firmware" ]; then
        echo "$firmware: no such directory; see Documentation/RaspberryPi.md for what goes in it" >&2
        return 1
    fi
    if [ ! -f "$build/raspberry-pi/kernel8.img" ]; then
        echo "$build/raspberry-pi/kernel8.img is missing; build first with: ninja -C $build" >&2
        return 1
    fi
    cp "$build/raspberry-pi/kernel8.img" "$esp/kernel8.img"
    cat > "$esp/config.txt" <<'CONFIG'
# vultureOS, on a Raspberry Pi 4 or 5. (See Documentation/RaspberryPi.md.)
arm_64bit=1
kernel=kernel8.img
kernel_address=0x80000
disable_overscan=1
disable_splash=1
enable_uart=1
# For the kernel's output on the serial pins, a Pi 4 also needs its PL011 back from Bluetooth, with this overlay in
# overlays/ of the firmware directory:
#dtoverlay=disable-bt
CONFIG
    : > "$esp/cmdline.txt"
    raspberry_pi_files=(kernel8.img config.txt cmdline.txt)
    local file relative
    while IFS= read -r file; do
        relative="${file#"$firmware"/}"
        mkdir -p "$esp/$(dirname "$relative")"
        cp "$file" "$esp/$relative"
        raspberry_pi_files+=("$relative")
    done < <(find "$firmware" -type f)
}
