#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The test harness itself, with what Meta/run.sh's chats can do to a machine. A harness that can't fail proves nothing, so
# each thing that should fail is made to, and has to say why.
#
#   Meta/harness-test.sh [--build-dir DIR]

set -euo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: harness-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

out="$build_dir/harness-test"
mkdir -p "$out"
run="$source_dir/Meta/run.sh"
status=0
ok() { echo "harness-test: ok: $*"; }
failed() {
    echo "harness-test: FAIL: $*" >&2
    status=1
}

# Writes a chat with these lines, runs it, and returns how that went.
chat() {
    local name="$1"
    shift
    printf '%s\n' "$@" > "$out/$name.chat"
    "$run" --build-dir "$build_dir" --headless --chat "$out/$name.chat" --timeout "${chat_timeout:-120}" ${chat_arguments[@]+"${chat_arguments[@]}"} > "$out/$name.log" 2>&1
}

expect_pass() {
    local name="$1"
    shift
    if chat "$name" "$@"; then
        ok "$name"
    else
        failed "$name: see $out/$name.log"
    fi
}

# It must fail, saying this.
expect_failure() {
    local name="$1" saying="$2"
    shift 2
    if chat "$name" "$@"; then
        failed "$name: it should have failed; see $out/$name.log"
    elif grep -aqF -- "$saying" "$out/$name.log"; then
        ok "$name"
    else
        failed "$name: it failed, but didn't say \"$saying\"; see $out/$name.log"
    fi
}

chat_arguments=(--boot-args console)

# ---- Waiting, and how long.
started="$SECONDS"
expect_failure "a step that waits too long fails by itself, not at the end" "waited more than 3s" \
    "timeout 3" "expect this will never be said"
if [ $((SECONDS - started)) -gt 60 ]; then
    failed "it took $((SECONDS - started))s to notice, so the limit isn't working"
fi

# ---- Something that must never be seen.
expect_failure "what's forbidden fails the chat" "saw what must not be seen: vultureOS userland is up" \
    "forbid vultureOS userland is up" "expect this will never be said"

# ---- A kernel panic fails any chat, unless it's what the chat expects.
chat_arguments=(--boot-args "console fault=data-abort")
expect_failure "a kernel panic fails a chat at once" "saw what must not be seen: KERNEL PANIC" \
    "expect this will never be said"
expect_pass "a chat that expects a panic isn't failed by it" \
    "expect KERNEL PANIC: Data abort"
chat_arguments=(--boot-args console)

# ---- The reset button: the machine starts again, and the chat goes on.
expect_pass "reboot starts the machine again" \
    "timeout 60" \
    "expect vultureOS userland is up" \
    "reboot" \
    "expect UEFI firmware" \
    "expect vultureOS userland is up"

# ---- Telling QEMU things, and what it says.
expect_pass "qmp, and what it answered" \
    "expect vultureOS userland is up" \
    'qmp {"execute":"query-status"}' \
    'qmp-result "status": "running"'
expect_failure "qmp's answer has to have what's wanted in it" "doesn't have" \
    "expect vultureOS userland is up" \
    'qmp {"execute":"query-status"}' \
    'qmp-result nothing like this'
expect_failure "a command QEMU refuses fails the chat" "QEMU refused" \
    "expect vultureOS userland is up" \
    'qmp {"execute":"no-such-command"}'

# ---- Things running on this machine.
marker="$out/fixture-ran"
: > "$marker"
expect_pass "a command on this machine" \
    "expect vultureOS userland is up" \
    "host echo it-ran > '$marker'" \
    "host grep -q it-ran '$marker'"
expect_failure "one that fails fails the chat" "on this machine, this failed" \
    "expect vultureOS userland is up" \
    "host exit 3"
expect_pass "a server left running, then stopped" \
    "expect vultureOS userland is up" \
    "host-start sleeper exec sleep 300" \
    "pause 0.5" \
    'host [ -f "$VULTURE_RUN_DIR/fixture.sleeper.pid" ] && kill -0 "$(cat "$VULTURE_RUN_DIR/fixture.sleeper.pid")"' \
    "host-stop sleeper"
expect_failure "stopping one that isn't running fails the chat" "nothing called nobody is running" \
    "expect vultureOS userland is up" \
    "host-stop nobody"

# ---- Plugging things in, as far as QEMU is concerned: M20 is when the system notices.
chat_arguments=(--boot-args console -- -device qemu-xhci,id=hub0)
expect_pass "plug and unplug" \
    "expect vultureOS userland is up" \
    "plug thing usb-kbd bus=hub0.0" \
    'qmp {"execute":"qom-list","arguments":{"path":"/machine/peripheral"}}' \
    "qmp-result thing" \
    "unplug thing"
expect_failure "plugging in what doesn't exist fails the chat" "QEMU refused" \
    "expect vultureOS userland is up" \
    "plug thing no-such-device"

# ---- A finger, on a touch device: M22 is when the system does something with it.
chat_arguments=(--boot-args console -- -device virtio-multitouch-pci)
expect_pass "touch begins, moves and ends" \
    "expect vultureOS userland is up" \
    "touch begin 0 1000 1000" \
    "touch update 0 2000 1000" \
    "touch end 0 2000 1000"
chat_arguments=(--boot-args console)

# ---- Two heads, and a screenshot of each: M22 is when the system draws on the second.
chat_arguments=(--boot-args console)
head_one="$out/head-1.png"
if VULTURE_GPU_OUTPUTS=2 VULTURE_SCREENSHOT_DEVICE=gpu0 chat "a screenshot of the second head" \
    "expect vultureOS userland is up" \
    "display 1" \
    "screenshot $head_one"; then
    for _ in $(seq 1 50); do
        [ -s "$head_one" ] && break
        sleep 0.1
    done
    if [ -s "$head_one" ]; then
        ok "a screenshot of the second head"
    else
        failed "a screenshot of the second head: nothing was saved; see $out/a screenshot of the second head.log"
    fi
else
    failed "a screenshot of the second head: see $out/a screenshot of the second head.log"
fi

# ---- Finding things by what they say: failing properly, and saying what was there.
chat_arguments=(--boot-args uidesktop)
expect_failure "looking for what isn't there fails, and shows what is" "@ax dump tree screen" \
    "timeout 8" \
    "expect loginwindow: ready" \
    'ax-expect button "No Such Thing"'
expect_failure "a thing that's there but not as asked fails" "waited more than" \
    "timeout 8" \
    "expect loginwindow: ready" \
    'ax-expect button "Shut Down" disabled'
expect_failure "a thing that won't go fails" "waited more than" \
    "timeout 8" \
    "expect loginwindow: ready" \
    'ax-gone button "Shut Down"'
expect_failure "a step that isn't one is refused" "not a step" \
    "pointless"

if [ "$status" -eq 0 ]; then
    echo "harness-test: PASS"
else
    echo "harness-test: FAIL" >&2
fi
exit "$status"
