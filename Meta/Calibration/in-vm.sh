#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs a command inside the macOS 15 virtual machine (see README.md), in its logged-in session, and prints what it said. The
# shared folder is the guest's working directory, so anything put there is there, and anything left there is here.
#
#   Meta/Calibration/in-vm.sh [--directory DIR] COMMAND [--timeout SECONDS]

set -euo pipefail

directory="${VULTURE_CALIBRATION_VM:-$HOME/vulture-calibration-vm}"
timeout_seconds=300
command=""
while [ $# -gt 0 ]; do
    case "$1" in
        --directory) directory="$2"; shift 2 ;;
        --timeout) timeout_seconds="$2"; shift 2 ;;
        *) command="$1"; shift ;;
    esac
done
if [ -z "$command" ]; then
    echo "usage: in-vm.sh [--directory DIR] [--timeout SECONDS] COMMAND" >&2
    exit 2
fi
share="$directory/share/calibration"
mkdir -p "$share/requests" "$share/results"
name="request-$$-$RANDOM"
# Whole, or not at all: the guest looks for files ending in .sh.
printf '%s\n' "$command" > "$share/requests/$name.tmp"
mv "$share/requests/$name.tmp" "$share/requests/$name.sh"
deadline=$((SECONDS + timeout_seconds))
while [ ! -f "$share/results/$name.status" ]; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "in-vm: nothing came back in ${timeout_seconds}s. Is the virtual machine running and logged in, and has guest-setup.sh been run in it?" >&2
        exit 124
    fi
    sleep 0.5
done
cat "$share/results/$name.out"
exit "$(cat "$share/results/$name.status")"
