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
#                       "pointer X Y" moves the pointer (0 to 32767 across the
#                       screen), "button left down" presses a mouse button,
#                       "pause SECONDS" waits (as a person would before ^Z);
#                       blank lines and "#" comments are skipped. Succeeds
#                       once every step is done (and any --expect found)
#
# Environment: QEMU (the emulator), VULTURE_ACCEL (hvf, kvm or tcg),
# VULTURE_FIRMWARE_CODE and VULTURE_FIRMWARE_VARS (the UEFI firmware and its
# variable store template; found under Homebrew's or Debian's names
# otherwise), VULTURE_TCG_TIMEOUT_SCALE, VULTURE_AUDIO_WAV (record what the
# sound device plays to this file, instead of playing it), VULTURE_NET_OPTIONS
# (added to QEMU's -netdev user, as in hostfwd=tcp::8022-:22).

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
        --nvme) disk_interface="nvme,drive=root,serial=VULTURE0001"; shift ;;
        --image) image="$2"; use_disk=0; shift 2 ;;
        --kill) kill_power=1; shift ;;
        --kill-delay) kill_delay="$2"; shift 2 ;;
        --) shift; extra_args=("$@"); break ;;
        --chat) chat_file="$2"; shift 2 ;;
        -h|--help) sed -n '8,66p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

qemu="${QEMU:-qemu-system-aarch64}"
# (Another kernel can be booted by giving its own copy of this, as sanitized-kernel-test.sh does.)
esp_dir="${VULTURE_ESP_DIR:-$build_dir/esp}"

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

# QEMU's window shows the first display: the text console, unless booting to the
# window server.
display_devices=(-device ramfb -device "virtio-gpu-pci,id=gpu0,xres=$gpu_width,yres=$gpu_height")
if [ "$gui" -eq 1 ]; then
    display_devices=(-device "virtio-gpu-pci,id=gpu0,xres=$gpu_width,yres=$gpu_height" -device ramfb)
fi

# The sound device, if this QEMU has it, and where its sound goes: a file if asked, the
# machine's speakers for a person, nowhere for a test.
audio_args=()
if "$qemu" -device help 2>/dev/null | grep -q virtio-sound-pci; then
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
    audio_args=(-audiodev "$audio_backend" -device virtio-sound-pci,audiodev=sound0)
fi

args=(
    # acpi=off makes the firmware publish a device tree, which the kernel uses for hardware discovery.
    -machine virt,gic-version=3,acpi=off
    -smp 8
    -m 4G
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$firmware_code"
    -drive "if=pflash,format=raw,unit=1,file=$vars_file"
    "${boot_disk_args[@]}"
    # ramfb gives the firmware a linear framebuffer for the boot console (and the
    # text console); virtio-gpu only offers blit operations until the kernel's
    # driver takes it, and is what the window server draws on.
    "${display_devices[@]}"
    # Lets the firmware hand the kernel some entropy.
    -device virtio-rng-pci
    # USB has no driver yet (M14). These reach the text console and the window server.
    -device virtio-keyboard-pci
    -device virtio-tablet-pci
    ${audio_args[@]+"${audio_args[@]}"}
    # QEMU's user-mode network: a NAT, with DHCP and DNS of its own, and the host at 10.0.2.2.
    -netdev "user,id=net0${VULTURE_NET_OPTIONS:+,$VULTURE_NET_OPTIONS}"
    -device virtio-net-pci,netdev=net0
)

case "$accelerator" in
    hvf | kvm) args+=(-accel "$accelerator" -cpu host) ;;
    tcg) args+=(-accel tcg,thread=multi -cpu max,pauth-impdef=on) ;;
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
            "expect "* | "send "* | "pause "* | "type "* | "key "* | "pointer "* | "button "* | "screenshot "*) chat_steps+=("$line") ;;
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
trap 'kill "$qemu_pid" ${reader_pid:+"$reader_pid"} 2>/dev/null || true; rm -rf "$run_dir"' EXIT
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
    if [ -n "${VULTURE_SCREENSHOT_DEVICE:-}" ]; then
        printf '{"execute":"qmp_capabilities"}\n{"execute":"screendump","arguments":{"filename":"%s","format":"png","device":"%s"}}\n' "$file" "$VULTURE_SCREENSHOT_DEVICE"
    else
        printf '{"execute":"qmp_capabilities"}\n{"execute":"screendump","arguments":{"filename":"%s","format":"png"}}\n' "$file"
    fi |
        nc -U -w 1 "$qmp_socket" > /dev/null || echo "run.sh: screenshot failed" >&2
}

# The chat: which step is next, and where in the log the last expected
# text ended (byte offsets, so no locale may count characters instead).
chat_index=0
chat_offset=0
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
            "button "*)
                press_button ${step#button }
                chat_index=$((chat_index + 1))
                ;;
            "pause "*)
                sleep "${step#pause }"
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
        esac
    done
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
# What the guest said last, before it powered off, counts too.
if [ ${#chat_steps[@]} -gt 0 ]; then
    advance_chat
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
    if chat_done; then
        echo "run.sh: chat: all ${#chat_steps[@]} steps done"
    else
        echo "run.sh: chat: stuck at step $((chat_index + 1)) of ${#chat_steps[@]}: ${chat_steps[$chat_index]}" >&2
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
