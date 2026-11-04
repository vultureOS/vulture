#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Boots vultureOS in QEMU with UEFI firmware: its AArch64 "virt" machine, or x86_64 "q35",
# whichever the build directory is for.
#
#   Meta/run.sh [options] [-- extra QEMU arguments]
#
# With no options, the desktop in QEMU's window and nothing in this terminal (what the
# system says goes to <build>/qemu/console.log). Meta/run_debug.sh is that with all of
# it here; Meta/run_console.sh boots to the text console instead.
#
#   --build-dir DIR     build directory (default: <repo>/build)
#   --boot-args TEXT    kernel command line, e.g. "-v" for a verbose boot
#   --gui               boot to the desktop: the default, but with --headless
#   --console           boot to the text console (in the window and this terminal)
#   --debug             show what the system is doing in this terminal: the kernel, every
#                       process and job started and ended, the unified log at every
#                       level. A log-in prompt too. Kept in <build>/qemu/debug.log, with
#                       what QEMU notices the guest doing wrong in <build>/qemu/qemu.log
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
#   --memory SIZE       how much memory the machine has (default: 4G)
#   --raspberry-pi      QEMU's Raspberry Pi 4, started from kernel8.img as the Pi's own firmware
#                       would, with the disk as its SD card. (It's only emulated, so it's slow, and it
#                       has no USB or network here. --image gives the SD card, instead of a new one.)
#   --real-devices      the kinds of device real machines have, in place of virtio: an AHCI
#                       disk, xHCI USB with a keyboard and tablet, e1000e network, Intel
#                       HD Audio, and only the firmware's framebuffer to draw on.
#   --nvme              attach the root disk as NVMe instead of virtio-blk
#   --image FILE        boot a whole-disk image (ninja image) on its own,
#                       instead of the build's EFI directory and root disk;
#                       boot arguments do not apply
#   --kill              with --headless: once the expected text appears, cut
#                       the power (SIGKILL) instead of shutting QEMU down
#   --kill-delay SECONDS  with --kill: wait this long first (may be fractional)
#   --chat FILE         with --headless: type at the serial console as FILE
#                       says, one step a line: "expect TEXT" waits for TEXT
#                       (after what the last expect matched), "send TEXT"
#                       types TEXT (with \r, \n, \t and \xHH escapes),
#                       "type TEXT" types it on the keyboard instead (\r for
#                       Return), "key KEYS" presses keys there (up, ctrl-c),
#                       "screenshot FILE" saves the screen there, with
#                       $VULTURE_SCREENSHOT_PREFIX before it,
#                       "nudge DX DY" moves the pointer by that much, as a mouse does,
#                       "pointer X Y" moves the pointer (0 to 32767 across the
#                       screen), "button left down" presses a mouse button,
#                       "pause SECONDS" waits (as a person would before ^Z);
#                       blank lines and "#" comments are skipped. Succeeds
#                       once every step is done (and any --expect found).
#
#                       By what things say rather than where they are (booted with
#                       uidesktop, which runs "ax --serve"; a ROLE is optional):
#                       'click ROLE "Label"' presses it with the real pointer,
#                       'ax-expect ROLE "Label"' waits for it, optionally followed by
#                       "enabled", "disabled", "focused", "= VALUE" or "~ PART OF VALUE", and
#                       'ax-gone ROLE "Label"' waits until it's gone.
#
#                       Everything else a test might need to do to the machine:
#                       "timeout SECONDS" limits how long each later step may wait
#                       (0: only --timeout does), "forbid TEXT" fails the chat at once if
#                       TEXT is ever seen ("KERNEL PANIC" always is, unless expected),
#                       "reboot" presses the reset button, "plug ID DRIVER [PROPERTIES]"
#                       and "unplug ID" add and remove a device, "qmp JSON" tells QEMU
#                       anything else (a failure fails the chat), "qmp-result TEXT" says
#                       TEXT must be in its answer, "touch begin|update|end SLOT X Y" is
#                       a finger on a touch device, "display HEAD" says which head
#                       later screenshots are of, "host COMMAND" runs a command on this
#                       machine (a failure fails the chat), "host-start NAME COMMAND"
#                       leaves one running, as a server for the guest to talk to, until
#                       "host-stop NAME" or the end (see $VULTURE_RUN_DIR, a directory
#                       for this run).
#
# Environment: QEMU (the emulator), VULTURE_ACCEL (hvf, kvm or tcg),
# VULTURE_FIRMWARE_CODE and VULTURE_FIRMWARE_VARS (the UEFI firmware and its
# variable store template; found under Homebrew's or Debian's names
# otherwise), VULTURE_TCG_TIMEOUT_SCALE, VULTURE_AUDIO_WAV (record what the
# sound device plays to this file, instead of playing it), VULTURE_NET_OPTIONS
# (added to QEMU's -netdev user, as in hostfwd=tcp::8022-:22), VULTURE_GPU_OUTPUTS
# (how many displays the GPU has), VULTURE_STEP_TIMEOUT (a chat's "timeout" to start with), VULTURE_IMAGE_ON (what
# --image is attached by: virtio, nvme, ahci or usb).

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$root_dir/build"
boot_args=""
gui=""
debug=0
accelerator="${VULTURE_ACCEL:-}"
# QEMU's own default, so that its window is a comfortable size to work beside. (A window
# takes the size of its display: VULTURE_GPU_WIDTH=2560 VULTURE_GPU_HEIGHT=1600 gives the
# 2x look, in a window that fills most laptops' screens.)
gpu_width="${VULTURE_GPU_WIDTH:-1280}"
gpu_height="${VULTURE_GPU_HEIGHT:-800}"
gdb=0
headless=0
expectations=()
timeout_seconds=60
wait_for_exit=0
screenshot=""
disk=""
image=""
memory_size=4G
# (From the environment too, for a test that boots several times.)
real_devices="${VULTURE_REAL_DEVICES:-0}"
raspberry_pi=0
use_disk=1
fresh_disk=0
disk_interface="virtio-blk-pci,drive=root"
kill_power=0
kill_delay=0
chat_file=""
extra_args=()

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --boot-args) boot_args="$2"; shift 2 ;;
        --gui) gui=1; shift ;;
        --console) gui=0; shift ;;
        --debug) debug=1; shift ;;
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
        --memory) memory_size="$2"; shift 2 ;;
        --real-devices) real_devices=1; shift ;;
        --nvme) disk_interface="nvme,drive=root,serial=VULTURE0001"; shift ;;
        --image) image="$2"; use_disk=0; shift 2 ;;
        --raspberry-pi) raspberry_pi=1; shift ;;
        --kill) kill_power=1; shift ;;
        --kill-delay) kill_delay="$2"; shift 2 ;;
        --) shift; extra_args=("$@"); break ;;
        --chat) chat_file="$2"; shift 2 ;;
        -h|--help) sed -n '8,95p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "run.sh: unknown option '$1' (see --help)" >&2; exit 2 ;;
    esac
done

# The desktop, for a person; the console, for what scripts talk to, unless they say.
if [ -z "$gui" ]; then
    if [ "$headless" -eq 1 ]; then gui=0; else gui=1; fi
fi
if [ -n "$image" ] && { [ "$debug" -eq 1 ] || [ "$gui" -eq 0 ]; }; then
    echo "run.sh: an image has no boot arguments: it boots to the desktop, without debug logs" >&2
fi

# What the build directory is for.
architecture="$(sed -n 's/^VULTURE_ARCH:[A-Z]*=//p' "$build_dir/CMakeCache.txt" 2>/dev/null || true)"
architecture="${architecture:-aarch64}"
qemu="${QEMU:-qemu-system-$architecture}"
# (Another kernel can be booted by giving its own copy of this, as sanitized-kernel-test.sh does.)
esp_dir="${VULTURE_ESP_DIR:-$build_dir/esp}"

# The hypervisor where there is one: HVF on an Apple silicon Mac that offers
# it (not inside most virtual machines, CI runners included), KVM on AArch64
# Linux; software emulation (TCG) everywhere else.
[ "$raspberry_pi" -eq 1 ] && accelerator="tcg"
if [ -z "$accelerator" ]; then
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64)
            if [ "$architecture" = aarch64 ] && [ "$(sysctl -n kern.hv_support 2>/dev/null)" = "1" ]; then accelerator="hvf"; else accelerator="tcg"; fi ;;
        Linux-aarch64 | Linux-x86_64)
            if [ "$(uname -m)" = "$architecture" ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then accelerator="kvm"; else accelerator="tcg"; fi ;;
        *) accelerator="tcg" ;;
    esac
fi
# Emulated, everything takes several times as long.
chat_pause_scale=1
chat_timeout_scale=1
if [ "$accelerator" = "tcg" ]; then
    timeout_seconds=$((timeout_seconds * ${VULTURE_TCG_TIMEOUT_SCALE:-8}))
    chat_timeout_scale="${VULTURE_TCG_TIMEOUT_SCALE:-8}"
    # (Including what a chat is pausing for: an animation that takes the time it takes.)
    chat_pause_scale="${VULTURE_TCG_PAUSE_SCALE:-4}"
fi

# The UEFI firmware for QEMU's virt machine and the template of its variable
# store: QEMU's own copies (Homebrew, or a QEMU built from source), or the
# AAVMF ones Debian and Ubuntu package (qemu-efi-aarch64).
firmware_code="${VULTURE_FIRMWARE_CODE:-}"
firmware_vars="${VULTURE_FIRMWARE_VARS:-}"
if [ "$raspberry_pi" -eq 0 ] && { [ -z "$firmware_code" ] || [ -z "$firmware_vars" ]; }; then
    if [ "$architecture" = x86_64 ]; then
        candidates=(
            "${VULTURE_FIRMWARE_DIR:-/nonexistent}/edk2-x86_64-code.fd:${VULTURE_FIRMWARE_DIR:-/nonexistent}/edk2-i386-vars.fd"
            "/opt/homebrew/share/qemu/edk2-x86_64-code.fd:/opt/homebrew/share/qemu/edk2-i386-vars.fd"
            "/usr/local/share/qemu/edk2-x86_64-code.fd:/usr/local/share/qemu/edk2-i386-vars.fd"
            "/usr/share/qemu/edk2-x86_64-code.fd:/usr/share/qemu/edk2-i386-vars.fd"
            "/usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd"
            "/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd"
        )
    else
        candidates=(
            "${VULTURE_FIRMWARE_DIR:-/nonexistent}/edk2-aarch64-code.fd:${VULTURE_FIRMWARE_DIR:-/nonexistent}/edk2-arm-vars.fd"
            "/opt/homebrew/share/qemu/edk2-aarch64-code.fd:/opt/homebrew/share/qemu/edk2-arm-vars.fd"
            "/usr/local/share/qemu/edk2-aarch64-code.fd:/usr/local/share/qemu/edk2-arm-vars.fd"
            "/usr/share/qemu/edk2-aarch64-code.fd:/usr/share/qemu/edk2-arm-vars.fd"
            "/usr/share/AAVMF/AAVMF_CODE.fd:/usr/share/AAVMF/AAVMF_VARS.fd"
        )
    fi
    for candidate in "${candidates[@]}"; do
        if [ -f "${candidate%%:*}" ] && [ -f "${candidate#*:}" ]; then
            firmware_code="${candidate%%:*}"
            firmware_vars="${candidate#*:}"
            break
        fi
    done
fi
if [ "$raspberry_pi" -eq 0 ] && { [ -z "$firmware_code" ] || [ ! -f "$firmware_code" ] || [ ! -f "$firmware_vars" ]; }; then
    echo "run.sh: no $architecture UEFI firmware found; install QEMU (brew install qemu, or apt install qemu-efi-aarch64 or ovmf)" >&2
    echo "        or set VULTURE_FIRMWARE_CODE and VULTURE_FIRMWARE_VARS" >&2
    exit 1
fi
state_dir="$build_dir/qemu"
kernels_dir="$esp_dir/System/Library/Kernels"

boot_file=BOOTAA64.EFI
[ "$architecture" = x86_64 ] && boot_file=BOOTX64.EFI
for required in "$esp_dir/EFI/BOOT/$boot_file" "$kernels_dir/vulture"; do
    if [ ! -f "$required" ]; then
        echo "run.sh: $required is missing; build first with: ninja -C $build_dir" >&2
        exit 1
    fi
done

# VultureBoot passes the contents of boot-args to the kernel as its command line.
# (The kernel boots to the desktop unless told otherwise, as an image must. And with a debugger
# attached, it stays where it was linked, so that the symbols mean what they say.)
if [ "$gdb" -eq 1 ]; then
    boot_args="${boot_args:+$boot_args }nokaslr"
fi
if [ "$gui" -eq 0 ]; then
    boot_args="${boot_args:+$boot_args }console"
fi
if [ "$debug" -eq 1 ]; then
    boot_args="${boot_args:+$boot_args }debug"
fi
# QEMU's own window on a Mac goes by the corner of the cursor's picture, and takes no notice of where in it the
# pointer's meant to be: tell the kernel, or what looks like the tip of the arrow is 6 points from where it clicks.
if [ "$headless" -eq 0 ] && [ "$(uname -s)" = Darwin ]; then
    case " ${extra_args[*]-} " in
        *" -display "*) ;;
        *) boot_args="${boot_args:+$boot_args }cursortopleft" ;;
    esac
fi
if [ -n "$boot_args" ]; then
    printf '%s\n' "$boot_args" > "$kernels_dir/boot-args"
else
    rm -f "$kernels_dir/boot-args"
fi

# The firmware keeps its settings (boot order etc.) in a writable variable store.
mkdir -p "$state_dir"
if [ "$raspberry_pi" -eq 0 ] && [ ! -f "$state_dir/efivars.fd" ]; then
    cp "$firmware_vars" "$state_dir/efivars.fd"
fi
vars_file="$state_dir/efivars.fd"

# Headless runs (tests) each get their own directory and copy of the
# variable store, so several can run at once.
run_dir=""
if [ "$headless" -eq 1 ]; then
    run_dir="$(mktemp -d "$state_dir/run.XXXXXX")"
    [ "$raspberry_pi" -eq 1 ] || cp "$state_dir/efivars.fd" "$run_dir/efivars.fd"
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
    if [ "$real_devices" -eq 1 ]; then
        disk_args=(
            -drive "if=none,id=root,format=raw,file=$disk,cache=writeback"
            -device ich9-ahci,id=ahci0
            -device ide-hd,drive=root,bus=ahci0.0
        )
    else
        disk_args=(
            -drive "if=none,id=root,format=raw,file=$disk,cache=writeback"
            -device "$disk_interface"
        )
    fi
fi

# Either one whole disk (EFI System Partition and vultureFS), or the build's
# EFI directory as a read-only FAT disk plus the root disk.
if [ -n "$image" ]; then
    if [ ! -f "$image" ]; then
        echo "run.sh: $image is missing; make it with: ninja -C $build_dir image" >&2
        exit 1
    fi
    # What it's attached by: VULTURE_IMAGE_ON says, for a test of them all.
    case "${VULTURE_IMAGE_ON:-}" in
        ahci) image_device=(-device ich9-ahci,id=ahci0 -device ide-hd,drive=image,bus=ahci0.0,bootindex=0) ;;
        nvme) image_device=(-device nvme,drive=image,serial=VULTURE0001,bootindex=0) ;;
        usb) image_device=(-device qemu-xhci,id=usb-stick -device usb-storage,bus=usb-stick.0,drive=image,bootindex=0) ;;
        virtio) image_device=(-device virtio-blk-pci,drive=image,bootindex=0) ;;
        *) image_device=(-device "${disk_interface/drive=root/drive=image},bootindex=0") ;;
    esac
    boot_disk_args=(
        -drive "if=none,id=image,format=raw,file=$image,cache=writeback"
        "${image_device[@]}"
    )
else
    boot_disk_args=(
        -drive "if=none,id=esp,format=raw,readonly=on,file=fat:$esp_dir"
        -device virtio-blk-pci,drive=esp,bootindex=0
        ${disk_args[@]+"${disk_args[@]}"}
    )
fi

# QEMU's window shows the first display: the text console, unless booting to the
# window server.
gpu_device="virtio-gpu-pci,id=gpu0,xres=$gpu_width,yres=$gpu_height${VULTURE_GPU_OUTPUTS:+,max_outputs=$VULTURE_GPU_OUTPUTS}"
display_devices=(-device ramfb -device "$gpu_device")
if [ "$gui" -eq 1 ]; then
    display_devices=(-device "$gpu_device" -device ramfb)
fi
# A real machine's display is whatever the firmware set up. (A test can keep the one it knows the size
# of, for what it needs from it.)
if [ "$real_devices" -eq 1 ] && [ -z "${VULTURE_KEEP_VIRTIO_DISPLAY:-}" ]; then
    display_devices=(-device ramfb)
fi

# The sound device, if this QEMU has it, and where its sound goes: a file if asked, the
# machine's speakers for a person, nowhere for a test.
audio_args=()
if [ "$real_devices" -eq 1 ] || "$qemu" -device help 2>/dev/null | grep -q virtio-sound-pci; then
    audio_backend="none,id=sound0"
    if [ -n "${VULTURE_AUDIO_WAV:-}" ]; then
        audio_backend="wav,id=sound0,path=$VULTURE_AUDIO_WAV,out.frequency=48000,out.channels=2,out.format=s16"
    elif [ "$headless" -eq 0 ]; then
        backends="$("$qemu" -audiodev help 2>/dev/null || true)"
        for candidate in coreaudio pipewire pa alsa sdl; do
            if printf '%s\n' "$backends" | grep -qx "$candidate"; then
                audio_backend="$candidate,id=sound0"
                break
            fi
        done
    fi
    if [ "$real_devices" -eq 1 ]; then
        audio_args=(-audiodev "$audio_backend" -device ich9-intel-hda -device hda-duplex,audiodev=sound0)
    else
        audio_args=(-audiodev "$audio_backend" -device virtio-sound-pci,audiodev=sound0)
    fi
fi

if [ "$architecture" = x86_64 ]; then
    machine_args=(-machine q35)
else
    # acpi=off makes the firmware publish a device tree, which the kernel uses for hardware discovery.
    machine_args=(-machine virt,gic-version=3,acpi=off)
fi

if [ "$real_devices" -eq 1 ]; then
    if [ -n "${VULTURE_PS2_INPUT:-}" ]; then
        # The keyboard and mouse a PC has by itself: QEMU's q35 has them unless something else is added.
        input_devices=()
    else
        input_devices=(-device qemu-xhci,id=usb0 -device usb-kbd,bus=usb0.0 -device usb-tablet,bus=usb0.0)
    fi
    network_device=e1000e
else
    input_devices=(-device virtio-keyboard-pci -device virtio-tablet-pci)
    network_device=virtio-net-pci
fi

args=(
    "${machine_args[@]}"
    -smp 8
    -m "$memory_size"
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$firmware_code"
    -drive "if=pflash,format=raw,unit=1,file=$vars_file"
    "${boot_disk_args[@]}"
    # ramfb gives the firmware a linear framebuffer for the boot console (and the
    # text console); virtio-gpu only offers blit operations until the kernel's
    # driver takes it, and is what the window server draws on.
    "${display_devices[@]}"
    # Lets the firmware hand the kernel some entropy.
    -device virtio-rng-pci
    # These reach the text console and the window server.
    ${input_devices[@]+"${input_devices[@]}"}
    ${audio_args[@]+"${audio_args[@]}"}
    # QEMU's user-mode network: a NAT, with DHCP and DNS of its own, and the host at 10.0.2.2.
    -netdev "user,id=net0${VULTURE_NET_OPTIONS:+,$VULTURE_NET_OPTIONS}"
    -device "$network_device,netdev=net0"
)

# A Raspberry Pi is what it is: it starts from one file, and has its own idea of devices.
if [ "$raspberry_pi" -eq 1 ]; then
    if [ "$architecture" != aarch64 ]; then
        echo "run.sh: a Raspberry Pi needs a build for aarch64" >&2
        exit 2
    fi
    pi_dir="$build_dir/raspberry-pi"
    for required in "$pi_dir/kernel8.img" "$pi_dir/raspi4b.dtb"; do
        if [ ! -f "$required" ]; then
            echo "run.sh: $required is missing; build first with: ninja -C $build_dir" >&2
            exit 1
        fi
    done
    args=(
        -machine raspi4b
        -global "bcm2835-fb.xres=$gpu_width"
        -global "bcm2835-fb.yres=$gpu_height"
        -kernel "$pi_dir/kernel8.img"
        -dtb "$pi_dir/raspi4b.dtb"
        -append "$boot_args"
        -drive "if=sd,format=raw,file=${image:-$disk}"
    )
fi

case "$accelerator" in
    hvf | kvm) args+=(-accel "$accelerator" -cpu host) ;;
    tcg)
        if [ "$raspberry_pi" -eq 1 ]; then
            args+=(-accel tcg,thread=multi)
        elif [ "$architecture" = x86_64 ]; then
            args+=(-accel tcg,thread=multi -cpu max)
        else
            args+=(-accel tcg,thread=multi -cpu max,pauth-impdef=on)
        fi ;;
    *) echo "run.sh: unknown accelerator '$accelerator' (hvf, kvm or tcg)" >&2; exit 2 ;;
esac

if [ "$gdb" -eq 1 ]; then
    args+=(-s -S)
    echo "run.sh: waiting for a debugger on localhost:1234" >&2
fi

if [ "$headless" -eq 0 ]; then
    mkdir -p "$state_dir"
    if [ "$debug" -eq 1 ]; then
        echo "run.sh: debug logs follow, and are kept in $state_dir/debug.log (QEMU's own in qemu.log)." >&2
        echo "run.sh: this terminal is the serial console too: log in here for ps, log show, and so on." >&2
        exec "$qemu" "${args[@]}" -d guest_errors,unimp -D "$state_dir/qemu.log" \
            -chardev "stdio,id=console,signal=on,logfile=$state_dir/debug.log" -serial chardev:console \
            ${extra_args[@]+"${extra_args[@]}"}
    fi
    if [ "$gui" -eq 1 ]; then
        echo "run.sh: vultureOS is starting in QEMU's window. Log in as vulture, password vulture." >&2
        echo "run.sh: its console output goes to $state_dir/console.log; Meta/run_debug.sh shows it all here." >&2
        exec "$qemu" "${args[@]}" -serial "file:$state_dir/console.log" ${extra_args[@]+"${extra_args[@]}"}
    fi
    exec "$qemu" "${args[@]}" -serial stdio ${extra_args[@]+"${extra_args[@]}"}
fi

log="$run_dir/serial.log"
: > "$log"
qmp_socket="$run_dir/qmp.sock"
rm -f "$qmp_socket"

# The serial console: a file, or, to type at it (--chat), a pair of FIFOs
# (QEMU's "pipe" backend) whose output is copied into the log.
serial_args=(-serial "file:$log")
reader_pid=""
chat_steps=()
if [ -n "$chat_file" ]; then
    if [ ! -f "$chat_file" ]; then
        echo "run.sh: $chat_file is missing" >&2
        exit 2
    fi
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            "" | "#"*) ;;
            "expect "* | "send "* | "pause "* | "type "* | "key "* | "pointer "* | "nudge "* | "button "* | "screenshot "* | \
                "click "* | "ax-expect "* | "ax-gone "* | "timeout "* | "forbid "* | "reboot" | "plug "* | "unplug "* | "qmp "* | "qmp-result "* | \
                "touch "* | "display "* | "host "* | "host-start "* | "host-stop "*) chat_steps+=("$line") ;;
            *) echo "run.sh: $chat_file: not a step: $line" >&2; exit 2 ;;
        esac
    done < "$chat_file"
    mkfifo "$run_dir/serial.in" "$run_dir/serial.out"
    cat "$run_dir/serial.out" >> "$log" &
    reader_pid=$!
    serial_args=(-serial "pipe:$run_dir/serial")
fi

"$qemu" "${args[@]}" -display none -monitor none "${serial_args[@]}" -qmp "unix:$qmp_socket,server=on,wait=off" \
    ${extra_args[@]+"${extra_args[@]}"} &
qemu_pid=$!
# Whatever a chat left running on this machine.
stop_fixtures() {
    local pid_file
    for pid_file in "$run_dir"/fixture.*.pid; do
        [ -f "$pid_file" ] || continue
        kill "$(cat "$pid_file")" 2>/dev/null || true
        rm -f "$pid_file"
    done
}
trap 'stop_fixtures; kill "$qemu_pid" ${reader_pid:+"$reader_pid"} 2>/dev/null || true; rm -rf "$run_dir"' EXIT
if [ -n "$chat_file" ]; then
    # Read and write, so opening never waits for QEMU.
    exec 7<> "$run_dir/serial.in"
fi

# Presses keys on the virtual keyboard, through QMP: each argument one press,
# with others held, as ctrl-c. (A chat step: "key up", "type some text\\r".)
press_keys() {
    local commands='{"execute":"qmp_capabilities"}' combo keys key
    for combo in "$@"; do
        keys=""
        while [ -n "$combo" ]; do
            key="${combo%%-*}"
            if [ "$key" = "$combo" ]; then combo=""; else combo="${combo#*-}"; fi
            keys="$keys${keys:+,}{\"type\":\"qcode\",\"data\":\"$key\"}"
        done
        commands="$commands
{\"execute\":\"send-key\",\"arguments\":{\"keys\":[$keys],\"hold-time\":10}}"
    done
    printf '%s\n' "$commands" | nc -U -w 1 "$qmp_socket" > /dev/null || echo "run.sh: QMP failed" >&2
}

send_input_events() {
    printf '{"execute":"qmp_capabilities"}\n{"execute":"input-send-event","arguments":{"events":[%s]}}\n' "$1" |
        nc -U -w 1 "$qmp_socket" > /dev/null || echo "run.sh: QMP failed" >&2
}

# The pointer, at this fraction of the screen, in 32768ths.
move_pointer() {
    send_input_events "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$1}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$2}}"
}

# By how much, as a mouse would.
nudge_pointer() {
    send_input_events "{\"type\":\"rel\",\"data\":{\"axis\":\"x\",\"value\":$1}},{\"type\":\"rel\",\"data\":{\"axis\":\"y\",\"value\":$2}}"
}

# "left down", "left up".
press_button() {
    local down=false
    [ "$2" = down ] && down=true
    send_input_events "{\"type\":\"btn\",\"data\":{\"down\":$down,\"button\":\"$1\"}}"
}

# The keys for one character of text, on a US keyboard.
key_for() {
    case "$1" in
        [a-z0-9]) printf '%s' "$1" ;;
        [A-Z]) printf 'shift-%s' "$(printf '%s' "$1" | tr 'A-Z' 'a-z')" ;;
        ' ') printf 'spc' ;;
        -) printf 'minus' ;;
        _) printf 'shift-minus' ;;
        .) printf 'dot' ;;
        ,) printf 'comma' ;;
        /) printf 'slash' ;;
        =) printf 'equal' ;;
        +) printf 'shift-equal' ;;
        ';') printf 'semicolon' ;;
        :) printf 'shift-semicolon' ;;
        "'") printf 'apostrophe' ;;
        '"') printf 'shift-apostrophe' ;;
        '|') printf 'shift-backslash' ;;
        '\') printf 'backslash' ;;
        '[') printf 'bracket_left' ;;
        ']') printf 'bracket_right' ;;
        '{') printf 'shift-bracket_left' ;;
        '}') printf 'shift-bracket_right' ;;
        '`') printf 'grave_accent' ;;
        '~') printf 'shift-grave_accent' ;;
        '^') printf 'shift-6' ;;
        '>') printf 'shift-dot' ;;
        '<') printf 'shift-comma' ;;
        '!') printf 'shift-1' ;;
        '?') printf 'shift-slash' ;;
        '@') printf 'shift-2' ;;
        '#') printf 'shift-3' ;;
        '$') printf 'shift-4' ;;
        '%') printf 'shift-5' ;;
        '&') printf 'shift-7' ;;
        '*') printf 'shift-8' ;;
        '(') printf 'shift-9' ;;
        ')') printf 'shift-0' ;;
        *) echo "run.sh: no key for '$1'" >&2 ;;
    esac
}

# Text at the keyboard; \\r presses Return.
type_on_keyboard() {
    local text="$1" presses=() character
    while [ -n "$text" ]; do
        case "$text" in
            '\r'*)
                presses+=(ret)
                text="${text#??}"
                ;;
            *)
                character="${text%"${text#?}"}"
                text="${text#?}"
                presses+=("$(key_for "$character")")
                ;;
        esac
    done
    press_keys "${presses[@]}"
    # QEMU takes them in its own time, each held 10 ms and let go: it is not finished
    # typing for a while yet, and the next step must not overtake it.
    sleep "$(awk "BEGIN { print ${#presses[@]} * 0.03 }")"
}

# Saves the screen as PNG: QEMU's machine protocol, negotiating capabilities and
# then dumping it. (QEMU writes it as it gets around to it.)
save_screen() {
    local file="$1"
    case "$file" in
        /*) ;;
        *) file="$PWD/$file" ;;
    esac
    if [ -n "$chat_head" ]; then
        printf '{"execute":"qmp_capabilities"}\n{"execute":"screendump","arguments":{"filename":"%s","format":"png","device":"%s","head":%s}}\n' "$file" "${VULTURE_SCREENSHOT_DEVICE:-gpu0}" "$chat_head"
    elif [ -n "${VULTURE_SCREENSHOT_DEVICE:-}" ]; then
        printf '{"execute":"qmp_capabilities"}\n{"execute":"screendump","arguments":{"filename":"%s","format":"png","device":"%s"}}\n' "$file" "$VULTURE_SCREENSHOT_DEVICE"
    else
        printf '{"execute":"qmp_capabilities"}\n{"execute":"screendump","arguments":{"filename":"%s","format":"png"}}\n' "$file"
    fi |
        nc -U -w 1 "$qmp_socket" > /dev/null || echo "run.sh: screenshot failed" >&2
}

# Tells QEMU something, and fails the chat if it refuses. Its answer is kept for qmp-result.
qmp_command() {
    local answer
    answer="$(printf '{"execute":"qmp_capabilities"}\n%s\n' "$1" | nc -U -w 1 "$qmp_socket" 2>&1)" || true
    printf '%s\n' "$answer" > "$run_dir/qmp.last"
    case "$answer" in
        *'"error"'*)
            chat_failure="QEMU refused: $1: $(printf '%s' "$answer" | grep -a '"error"' | head -n 1)"
            return 1
            ;;
        *'"return"'*) return 0 ;;
        *)
            chat_failure="no answer from QEMU to: $1"
            return 1
            ;;
    esac
}

# "plug ID DRIVER [KEY=VALUE,...]" as QEMU's device_add.
plug_device() {
    local id="$1" driver="$2" properties="${3:-}" arguments pair
    arguments="\"id\":\"$id\",\"driver\":\"$driver\""
    while [ -n "$properties" ]; do
        pair="${properties%%,*}"
        if [ "$pair" = "$properties" ]; then properties=""; else properties="${properties#*,}"; fi
        arguments="$arguments,\"${pair%%=*}\":\"${pair#*=}\""
    done
    qmp_command "{\"execute\":\"device_add\",\"arguments\":{$arguments}}"
}

# A finger: "begin", "update" or "end", which slot it's in, and where (0 to 32767 across the screen). QEMU wants an axis on
# every event, though only those of type "data" mean anything by it.
touch_event() {
    local kind="$1" slot="$2" x="$3" y="$4" id=$((1000 + $2)) events=""
    case "$kind" in
        begin | update | end) ;;
        *)
            chat_failure="a finger can begin, update or end, not $kind"
            return 1
            ;;
    esac
    one_event() {
        printf '{"type":"mtt","data":{"type":"%s","slot":%s,"tracking-id":%s,"axis":"%s","value":%s}}' "$1" "$slot" "$id" "$2" "$3"
    }
    if [ "$kind" = end ]; then
        events="$(one_event end x "$x")"
    else
        events="$(one_event "$kind" x "$x"),$(one_event data x "$x"),$(one_event data y "$y")"
    fi
    qmp_command "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[$events]}}"
}

# A command on this machine, from the top of the source tree, with the run's own directory to work in.
run_on_host() {
    (cd "$root_dir" && VULTURE_RUN_DIR="$run_dir" VULTURE_BUILD_DIR="$build_dir" bash -c "$1") >> "$run_dir/host.log" 2>&1 || {
        chat_failure="on this machine, this failed: $1: $(tail -n 3 "$run_dir/host.log" | tr '\n' ' ')"
        return 1
    }
}

start_on_host() {
    local name="$1" command="$2"
    (cd "$root_dir" && VULTURE_RUN_DIR="$run_dir" VULTURE_BUILD_DIR="$build_dir" exec bash -c "$command") >> "$run_dir/host.log" 2>&1 &
    printf '%s\n' "$!" > "$run_dir/fixture.$name.pid"
}

stop_on_host() {
    local pid_file="$run_dir/fixture.$1.pid"
    if [ ! -f "$pid_file" ]; then
        chat_failure="nothing called $1 is running on this machine"
        return 1
    fi
    kill "$(cat "$pid_file")" 2>/dev/null || true
    rm -f "$pid_file"
}

# ---- Asking the guest what's on its screen: see Utilities/bin/ax.cpp.
ax_next_id=0
ax_pending=""
ax_asked_at=0
ax_reply=""

# What an ax step asks about: 'ROLE "Label" rest' or '"Label" rest'.
parse_ax_target() {
    local text="$1"
    ax_role='*'
    case "$text" in
        \"*) ;;
        *\ \"*)
            ax_role="${text%% *}"
            text="${text#* }"
            ;;
        *)
            chat_failure="not 'ROLE \"Label\"': $1"
            return 1
            ;;
    esac
    text="${text#\"}"
    ax_label="${text%%\"*}"
    ax_rest="${text#*\"}"
    ax_rest="${ax_rest# }"
}

# Asks, the first time; then true once the answer is back, in ax_reply. Asks again if nothing came, as the program serving
# this may have been starting up, or restarting.
ax_ask() {
    local answer offset
    if [ -z "$ax_pending" ] || [ $((SECONDS - ax_asked_at)) -ge 3 ]; then
        ax_next_id=$((ax_next_id + 1))
        ax_pending="$ax_next_id"
        ax_asked_at="$SECONDS"
        ax_log_offset="$(LC_ALL=C wc -c < "$log" | tr -d ' ')"
        printf '@ax %s %s\n' "$ax_pending" "$1" >&7
        return 1
    fi
    answer="$(tail -c +$((ax_log_offset + 1)) "$log" | LC_ALL=C grep -ao "@ax $ax_pending \(found\|none\|error\)[^"$'\r'"]*" | head -n 1)" || true
    [ -n "$answer" ] || return 1
    ax_pending=""
    ax_reply="${answer#@ax * }"
    return 0
}

# True if what was found is as this step wants: "enabled", "disabled", "focused" or "= VALUE".
ax_condition_holds() {
    local enabled="$1" focused="$2" value="$3"
    case "$ax_rest" in
        "") return 0 ;;
        enabled) [ "$enabled" = 1 ] ;;
        disabled) [ "$enabled" = 0 ] ;;
        focused) [ "$focused" = 1 ] ;;
        "= "*) [ "$value" = "${ax_rest#= }" ] ;;
        "~ "*) case "$value" in *"${ax_rest#\~ }"*) return 0 ;; *) return 1 ;; esac ;;
        *)
            chat_failure="not a condition: $ax_rest"
            return 1
            ;;
    esac
}

# One go at an ax step. True when it's finished, one way or the other.
ax_step() {
    local kind="$1" x y width height screen_width screen_height enabled focused value
    parse_ax_target "$2" || return 0
    ax_ask "find $ax_role $ax_label" || return 1
    case "$ax_reply" in
        error*)
            chat_failure="ax says: $ax_reply"
            return 0
            ;;
        none*)
            [ "$kind" = gone ] && return 0
            ax_pending=""
            return 1
            ;;
    esac
    read -r _ x y width height screen_width screen_height enabled focused value <<< "$ax_reply"
    case "$kind" in
        gone) return 1 ;;
        expect) ax_condition_holds "$enabled" "$focused" "$value" ;;
        click)
            if [ "$enabled" != 1 ]; then
                return 1
            fi
            # Its middle, as a fraction of the screen.
            move_pointer $(((x + width / 2) * 32768 / screen_width)) $(((y + height / 2) * 32768 / screen_height))
            sleep 0.2
            press_button left down
            sleep 0.1
            press_button left up
            return 0
            ;;
    esac
}

# The chat: which step is next, and where in the log the last expected
# text ended (byte offsets, so no locale may count characters instead).
chat_index=0
chat_offset=0
chat_failure=""
chat_head=""
chat_step_limit="${VULTURE_STEP_TIMEOUT:-0}"
chat_waiting_index=-1
chat_waiting_since=0
chat_forbidden=()
# Nothing a walkthrough does should panic the kernel. If one means to, it expects that in so many words.
for step in ${chat_steps[@]+"${chat_steps[@]}"}; do
    case "$step" in "expect "*"KERNEL PANIC"*) chat_expects_panic=1 ;; esac
done
if [ -n "$chat_file" ] && [ -z "${chat_expects_panic:-}" ]; then
    chat_forbidden+=("KERNEL PANIC")
fi
chat_done() {
    [ "$chat_index" -ge ${#chat_steps[@]} ]
}
advance_chat() {
    local step text match
    while ! chat_done; do
        step="${chat_steps[$chat_index]}"
        case "$step" in
            "send "*)
                printf '%b' "${step#send }" >&7
                chat_index=$((chat_index + 1))
                ;;
            "type "*)
                type_on_keyboard "${step#type }"
                chat_index=$((chat_index + 1))
                ;;
            "key "*)
                press_keys "${step#key }"
                chat_index=$((chat_index + 1))
                ;;
            "pointer "*)
                move_pointer ${step#pointer }
                chat_index=$((chat_index + 1))
                ;;
            "nudge "*)
                nudge_pointer ${step#nudge }
                chat_index=$((chat_index + 1))
                ;;
            "button "*)
                press_button ${step#button }
                chat_index=$((chat_index + 1))
                ;;
            "pause "*)
                # (Not the moments between the two clicks of a double click, which have a time limit.)
                sleep "$(awk "BEGIN { pause = ${step#pause }; if (pause < 0.3) print pause; else print pause * $chat_pause_scale }")"
                chat_index=$((chat_index + 1))
                ;;
            "screenshot "*)
                save_screen "${VULTURE_SCREENSHOT_PREFIX:-}${step#screenshot }"
                chat_index=$((chat_index + 1))
                ;;
            "expect "*)
                text="${step#expect }"
                match="$(tail -c +$((chat_offset + 1)) "$log" | LC_ALL=C grep -aboF -- "$text" | head -n 1)" || true
                [ -n "$match" ] || return 0
                chat_offset=$((chat_offset + ${match%%:*} + $(printf '%s' "$text" | LC_ALL=C wc -c)))
                chat_index=$((chat_index + 1))
                ;;
            "click "*)
                ax_step click "${step#click }" || return 0
                [ -n "$chat_failure" ] && return 0
                chat_index=$((chat_index + 1))
                ;;
            "ax-expect "*)
                ax_step expect "${step#ax-expect }" || return 0
                [ -n "$chat_failure" ] && return 0
                chat_index=$((chat_index + 1))
                ;;
            "ax-gone "*)
                ax_step gone "${step#ax-gone }" || return 0
                [ -n "$chat_failure" ] && return 0
                chat_index=$((chat_index + 1))
                ;;
            "timeout "*)
                chat_step_limit="${step#timeout }"
                chat_index=$((chat_index + 1))
                ;;
            "forbid "*)
                chat_forbidden+=("${step#forbid }")
                chat_index=$((chat_index + 1))
                ;;
            "reboot")
                qmp_command '{"execute":"system_reset"}' || return 0
                chat_index=$((chat_index + 1))
                ;;
            "plug "*)
                # shellcheck disable=SC2086
                plug_device ${step#plug } || return 0
                chat_index=$((chat_index + 1))
                ;;
            "unplug "*)
                qmp_command "{\"execute\":\"device_del\",\"arguments\":{\"id\":\"${step#unplug }\"}}" || return 0
                chat_index=$((chat_index + 1))
                ;;
            "qmp "*)
                qmp_command "${step#qmp }" || return 0
                chat_index=$((chat_index + 1))
                ;;
            "qmp-result "*)
                text="${step#qmp-result }"
                if ! grep -qF -- "$text" "$run_dir/qmp.last" 2>/dev/null; then
                    chat_failure="QEMU's last answer doesn't have \"$text\" in it: $(tail -n 1 "$run_dir/qmp.last" 2>/dev/null)"
                    return 0
                fi
                chat_index=$((chat_index + 1))
                ;;
            "touch "*)
                # shellcheck disable=SC2086
                touch_event ${step#touch } || return 0
                chat_index=$((chat_index + 1))
                ;;
            "display "*)
                chat_head="${step#display }"
                chat_index=$((chat_index + 1))
                ;;
            "host "*)
                run_on_host "${step#host }" || return 0
                chat_index=$((chat_index + 1))
                ;;
            "host-start "*)
                text="${step#host-start }"
                start_on_host "${text%% *}" "${text#* }"
                chat_index=$((chat_index + 1))
                ;;
            "host-stop "*)
                stop_on_host "${step#host-stop }" || return 0
                chat_index=$((chat_index + 1))
                ;;
        esac
    done
}

# True if something this chat says must never be seen has been.
saw_forbidden() {
    local text
    for text in ${chat_forbidden[@]+"${chat_forbidden[@]}"}; do
        if grep -aqF -- "$text" "$log"; then
            chat_failure="saw what must not be seen: $text"
            return 0
        fi
    done
    return 1
}

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
    if [ ${#chat_steps[@]} -gt 0 ]; then
        advance_chat
        # How long this step has been at it.
        if [ -z "$chat_failure" ] && ! chat_done; then
            if [ "$chat_waiting_index" -ne "$chat_index" ]; then
                chat_waiting_index="$chat_index"
                chat_waiting_since="$SECONDS"
            elif [ "$chat_step_limit" -gt 0 ] && [ $((SECONDS - chat_waiting_since)) -ge $((chat_step_limit * chat_timeout_scale)) ]; then
                chat_failure="waited more than $((chat_step_limit * chat_timeout_scale))s"
            fi
        fi
        saw_forbidden || true
        if [ -n "$chat_failure" ]; then
            break
        fi
    fi
    if [ "$wait_for_exit" -eq 0 ] && { [ ${#expectations[@]} -gt 0 ] || [ ${#chat_steps[@]} -gt 0 ]; } && all_found && chat_done; then
        break
    fi
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "run.sh: timed out after ${timeout_seconds}s" >&2
        break
    fi
    sleep 0.2
done

# Stuck looking for something on the screen: what is on it?
if [ ${#chat_steps[@]} -gt 0 ] && ! chat_done && kill -0 "$qemu_pid" 2>/dev/null; then
    case "${chat_steps[$chat_index]}" in
        "click "* | "ax-expect "* | "ax-gone "*)
            printf '@ax dump dump\n' >&7
            for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
                LC_ALL=C grep -aq '@ax dump end' "$log" && break
                sleep 0.25
            done
            ;;
    esac
fi

if [ -n "$screenshot" ] && kill -0 "$qemu_pid" 2>/dev/null; then
    save_screen "$screenshot"
fi

if [ "$kill_power" -eq 1 ] && kill -0 "$qemu_pid" 2>/dev/null; then
    sleep "$kill_delay"
    # Like pulling the plug: QEMU gets no chance to finish anything.
    kill -KILL "$qemu_pid" 2>/dev/null || true
fi
kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true
if [ -n "$reader_pid" ]; then
    # The copy ends with QEMU's end of the pipe.
    wait "$reader_pid" 2>/dev/null || true
fi
# What the guest said last, before it powered off, counts too. (Only what's written: QEMU is gone.)
if [ ${#chat_steps[@]} -gt 0 ] && [ -z "$chat_failure" ] && ! chat_done; then
    case "${chat_steps[$chat_index]}" in
        "expect "*) advance_chat ;;
    esac
fi

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
if [ ${#chat_steps[@]} -gt 0 ]; then
    if [ -f "$run_dir/host.log" ] && [ -s "$run_dir/host.log" ]; then
        echo "run.sh: what ran on this machine said:"
        cat "$run_dir/host.log"
    fi
    if chat_done && [ -z "$chat_failure" ]; then
        echo "run.sh: chat: all ${#chat_steps[@]} steps done"
    elif chat_done; then
        echo "run.sh: chat: $chat_failure" >&2
        status=1
    else
        echo "run.sh: chat: stuck at step $((chat_index + 1)) of ${#chat_steps[@]}: ${chat_steps[$chat_index]}${chat_failure:+: $chat_failure}" >&2
        status=1
    fi
fi
if [ ${#expectations[@]} -eq 0 ] && [ ${#chat_steps[@]} -eq 0 ]; then
    exit "$status"
fi
for text in ${expectations[@]+"${expectations[@]}"}; do
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
