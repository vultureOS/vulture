#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Boots vultureOS in QEMU's AArch64 "virt" machine with UEFI firmware.
#
#   Meta/run.sh [options] [-- extra QEMU arguments]
#
#   --build-dir DIR     build directory (default: <repo>/build)
#   --boot-args TEXT    kernel command line, e.g. "-v" for a verbose boot
#   --tcg               software emulation (TCG) instead of the hypervisor
#                       (HVF on macOS, KVM on AArch64 Linux), which is
#                       otherwise used where there is one
#   --gdb               wait for a debugger on localhost:1234
#   --headless          no window; the serial console is captured and printed
#   --expect TEXT       with --headless: TEXT must appear on the console
#                       (repeatable; succeeds once all have appeared)
#   --timeout SECONDS   with --headless: give up after SECONDS (default: 60;
#                       times VULTURE_TCG_TIMEOUT_SCALE, default 8, with TCG)
#   --wait-for-exit     with --headless: keep running until the guest powers
#                       off by itself; fail if it has not by the timeout
#   --screenshot FILE   with --headless: save the screen as PNG once the
#                       expected text has appeared (or at the timeout)
#   --disk FILE         the root disk (a vultureFS disk image), used in place.
#                       Default: build/qemu/disk.img, made on first use and
#                       updated with the system files of each new build;
#                       with --headless, a new disk for every run
#   --fresh-disk        make the root disk anew from the build first
#   --no-disk           no root disk: the root is the boot archive, in memory
#   --nvme              attach the root disk as NVMe instead of virtio-blk
#   --image FILE        boot a whole-disk image (ninja image) on its own,
#                       instead of the build's EFI directory and root disk;
#                       boot arguments do not apply
#   --kill              with --headless: once the expected text appears, cut
#                       the power (SIGKILL) instead of shutting QEMU down
#   --kill-delay SECONDS  with --kill: wait this long first (may be fractional)
#
# Environment: QEMU (the emulator), VULTURE_ACCEL (hvf, kvm or tcg),
# VULTURE_FIRMWARE_CODE and VULTURE_FIRMWARE_VARS (the UEFI firmware and its
# variable store template; found under Homebrew's or Debian's names
# otherwise), VULTURE_TCG_TIMEOUT_SCALE.

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$root_dir/build"
boot_args=""
accelerator="${VULTURE_ACCEL:-}"
gdb=0
headless=0
expectations=()
timeout_seconds=60
wait_for_exit=0
screenshot=""
disk=""
image=""
use_disk=1
fresh_disk=0
disk_interface="virtio-blk-pci,drive=root"
kill_power=0
kill_delay=0
extra_args=()

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --boot-args) boot_args="$2"; shift 2 ;;
        --tcg) accelerator="tcg"; shift ;;
        --gdb) gdb=1; shift ;;
        --headless) headless=1; shift ;;
        --expect) expectations+=("$2"); shift 2 ;;
        --timeout) timeout_seconds="$2"; shift 2 ;;
        --wait-for-exit) wait_for_exit=1; shift ;;
        --screenshot) screenshot="$2"; shift 2 ;;
        --disk) disk="$2"; shift 2 ;;
        --fresh-disk) fresh_disk=1; shift ;;
        --no-disk) use_disk=0; shift ;;
        --nvme) disk_interface="nvme,drive=root,serial=VULTURE0001"; shift ;;
        --image) image="$2"; use_disk=0; shift 2 ;;
        --kill) kill_power=1; shift ;;
        --kill-delay) kill_delay="$2"; shift 2 ;;
        --) shift; extra_args=("$@"); break ;;
        -h|--help) sed -n '8,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "run.sh: unknown option '$1' (see --help)" >&2; exit 2 ;;
    esac
done

qemu="${QEMU:-qemu-system-aarch64}"
esp_dir="$build_dir/esp"

# The hypervisor where there is one: HVF on an Apple silicon Mac that offers
# it (not inside most virtual machines, CI runners included), KVM on AArch64
# Linux; software emulation (TCG) everywhere else.
if [ -z "$accelerator" ]; then
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64)
            if [ "$(sysctl -n kern.hv_support 2>/dev/null)" = "1" ]; then accelerator="hvf"; else accelerator="tcg"; fi ;;
        Linux-aarch64)
            if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then accelerator="kvm"; else accelerator="tcg"; fi ;;
        *) accelerator="tcg" ;;
    esac
fi
# Emulated, everything takes several times as long.
if [ "$accelerator" = "tcg" ]; then
    timeout_seconds=$((timeout_seconds * ${VULTURE_TCG_TIMEOUT_SCALE:-8}))
fi

# The UEFI firmware for QEMU's virt machine and the template of its variable
# store: QEMU's own copies (Homebrew, or a QEMU built from source), or the
# AAVMF ones Debian and Ubuntu package (qemu-efi-aarch64).
firmware_code="${VULTURE_FIRMWARE_CODE:-}"
firmware_vars="${VULTURE_FIRMWARE_VARS:-}"
if [ -z "$firmware_code" ] || [ -z "$firmware_vars" ]; then
    for candidate in \
        "${VULTURE_FIRMWARE_DIR:-/nonexistent}/edk2-aarch64-code.fd:${VULTURE_FIRMWARE_DIR:-/nonexistent}/edk2-arm-vars.fd" \
        "/opt/homebrew/share/qemu/edk2-aarch64-code.fd:/opt/homebrew/share/qemu/edk2-arm-vars.fd" \
        "/usr/local/share/qemu/edk2-aarch64-code.fd:/usr/local/share/qemu/edk2-arm-vars.fd" \
        "/usr/share/qemu/edk2-aarch64-code.fd:/usr/share/qemu/edk2-arm-vars.fd" \
        "/usr/share/AAVMF/AAVMF_CODE.fd:/usr/share/AAVMF/AAVMF_VARS.fd"; do
        if [ -f "${candidate%%:*}" ] && [ -f "${candidate#*:}" ]; then
            firmware_code="${candidate%%:*}"
            firmware_vars="${candidate#*:}"
            break
        fi
    done
fi
if [ -z "$firmware_code" ] || [ ! -f "$firmware_code" ] || [ ! -f "$firmware_vars" ]; then
    echo "run.sh: no AArch64 UEFI firmware found; install QEMU (brew install qemu, or apt install qemu-efi-aarch64)" >&2
    echo "        or set VULTURE_FIRMWARE_CODE and VULTURE_FIRMWARE_VARS" >&2
    exit 1
fi
state_dir="$build_dir/qemu"
kernels_dir="$esp_dir/System/Library/Kernels"

for required in "$esp_dir/EFI/BOOT/BOOTAA64.EFI" "$kernels_dir/vulture"; do
    if [ ! -f "$required" ]; then
        echo "run.sh: $required is missing; build first with: ninja -C $build_dir" >&2
        exit 1
    fi
done

# VultureBoot passes the contents of boot-args to the kernel as its command line.
if [ -n "$boot_args" ]; then
    printf '%s\n' "$boot_args" > "$kernels_dir/boot-args"
else
    rm -f "$kernels_dir/boot-args"
fi

# The firmware keeps its settings (boot order etc.) in a writable variable store.
mkdir -p "$state_dir"
if [ ! -f "$state_dir/efivars.fd" ]; then
    cp "$firmware_vars" "$state_dir/efivars.fd"
fi
vars_file="$state_dir/efivars.fd"

# Headless runs (tests) each get their own directory and copy of the
# variable store, so several can run at once.
run_dir=""
if [ "$headless" -eq 1 ]; then
    run_dir="$(mktemp -d "$state_dir/run.XXXXXX")"
    cp "$state_dir/efivars.fd" "$run_dir/efivars.fd"
    vars_file="$run_dir/efivars.fd"
fi

# The root disk: a GPT disk image with a vultureFS partition, made by mkvfs
# from the boot archive. A disk kept between runs gets the system files of
# each new build (the user's own files stay), like a system update.
disk_args=()
if [ "$use_disk" -eq 1 ]; then
    mkvfs="$build_dir/host/Tools/vfs/mkvfs"
    archive="$kernels_dir/vulture.bootarchive"
    if [ ! -x "$mkvfs" ] || [ ! -f "$archive" ]; then
        echo "run.sh: $mkvfs or $archive is missing; build first with: ninja -C $build_dir" >&2
        exit 1
    fi
    if [ -z "$disk" ]; then
        if [ "$headless" -eq 1 ]; then
            disk="$run_dir/disk.img"
            fresh_disk=1
        else
            disk="$state_dir/disk.img"
        fi
    fi
    if [ "$fresh_disk" -eq 1 ] || [ ! -f "$disk" ]; then
        rm -f "$disk"
        "$mkvfs" create "$disk" 2048 "$archive" > /dev/null
        touch "$disk.updated"
    elif [ "$archive" -nt "$disk.updated" ]; then
        "$mkvfs" update "$disk" "$archive" > /dev/null
        touch "$disk.updated"
    fi
    disk_args=(
        -drive "if=none,id=root,format=raw,file=$disk,cache=writeback"
        -device "$disk_interface"
    )
fi

# Either one whole disk (EFI System Partition and vultureFS), or the build's
# EFI directory as a read-only FAT disk plus the root disk.
if [ -n "$image" ]; then
    if [ ! -f "$image" ]; then
        echo "run.sh: $image is missing; make it with: ninja -C $build_dir image" >&2
        exit 1
    fi
    boot_disk_args=(
        -drive "if=none,id=image,format=raw,file=$image,cache=writeback"
        -device "${disk_interface/drive=root/drive=image},bootindex=0"
    )
else
    boot_disk_args=(
        -drive "if=none,id=esp,format=raw,readonly=on,file=fat:$esp_dir"
        -device virtio-blk-pci,drive=esp,bootindex=0
        ${disk_args[@]+"${disk_args[@]}"}
    )
fi

args=(
    # acpi=off makes the firmware publish a device tree, which the kernel uses for hardware discovery.
    -machine virt,gic-version=3,acpi=off
    -smp 8
    -m 4G
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$firmware_code"
    -drive "if=pflash,format=raw,unit=1,file=$vars_file"
    "${boot_disk_args[@]}"
    # ramfb gives the firmware a linear framebuffer for the boot console; virtio-gpu
    # only offers blit operations until vultureOS has its own driver (M8).
    -device ramfb
    # Lets the firmware hand the kernel some entropy.
    -device virtio-rng-pci
    -device qemu-xhci
    -device usb-kbd
    -device usb-tablet
    -nic none
)

case "$accelerator" in
    hvf | kvm) args+=(-accel "$accelerator" -cpu host) ;;
    tcg) args+=(-accel tcg,thread=multi -cpu max) ;;
    *) echo "run.sh: unknown accelerator '$accelerator' (hvf, kvm or tcg)" >&2; exit 2 ;;
esac

if [ "$gdb" -eq 1 ]; then
    args+=(-s -S)
    echo "run.sh: waiting for a debugger on localhost:1234" >&2
fi

if [ "$headless" -eq 0 ]; then
    exec "$qemu" "${args[@]}" -serial stdio ${extra_args[@]+"${extra_args[@]}"}
fi

log="$run_dir/serial.log"
: > "$log"
qmp_socket="$run_dir/qmp.sock"
rm -f "$qmp_socket"
"$qemu" "${args[@]}" -display none -monitor none -serial "file:$log" -qmp "unix:$qmp_socket,server=on,wait=off" \
    ${extra_args[@]+"${extra_args[@]}"} &
qemu_pid=$!
trap 'kill "$qemu_pid" 2>/dev/null || true; rm -rf "$run_dir"' EXIT

# True once every expected text is in the log.
all_found() {
    local text
    for text in ${expectations[@]+"${expectations[@]}"}; do
        grep -qF -- "$text" "$log" || return 1
    done
    return 0
}

deadline=$((SECONDS + timeout_seconds))
exited_by_itself=0
while true; do
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        exited_by_itself=1
        break
    fi
    if [ "$wait_for_exit" -eq 0 ] && [ ${#expectations[@]} -gt 0 ] && all_found; then
        break
    fi
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "run.sh: timed out after ${timeout_seconds}s" >&2
        break
    fi
    sleep 0.2
done

if [ -n "$screenshot" ] && kill -0 "$qemu_pid" 2>/dev/null; then
    case "$screenshot" in
        /*) ;;
        *) screenshot="$PWD/$screenshot" ;;
    esac
    # QEMU's machine protocol: negotiate capabilities, then dump the screen.
    printf '{"execute":"qmp_capabilities"}\n{"execute":"screendump","arguments":{"filename":"%s","format":"png"}}\n' "$screenshot" |
        nc -U -w 2 "$qmp_socket" > /dev/null || echo "run.sh: screenshot failed" >&2
fi

if [ "$kill_power" -eq 1 ] && kill -0 "$qemu_pid" 2>/dev/null; then
    sleep "$kill_delay"
    # Like pulling the plug: QEMU gets no chance to finish anything.
    kill -KILL "$qemu_pid" 2>/dev/null || true
fi
kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true

# Show the console without the firmware's terminal control sequences.
sed -e $'s/\x1b\\[[0-9;?=]*[A-Za-z]//g' -e $'s/\r//g' "$log"

status=0
if [ "$wait_for_exit" -eq 1 ]; then
    if [ "$exited_by_itself" -eq 1 ]; then
        echo "run.sh: the guest powered off"
    else
        echo "run.sh: the guest did not power off" >&2
        status=1
    fi
fi
if [ ${#expectations[@]} -eq 0 ]; then
    exit "$status"
fi
for text in "${expectations[@]}"; do
    if grep -qF -- "$text" "$log"; then
        echo "run.sh: found \"$text\""
    else
        echo "run.sh: MISSING \"$text\"" >&2
        status=1
    fi
done
if [ "$status" -eq 0 ]; then
    echo "run.sh: PASS"
else
    echo "run.sh: FAIL" >&2
fi
exit "$status"
