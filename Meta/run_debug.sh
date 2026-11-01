#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The desktop, as Meta/run.sh boots it, with what the system is doing shown in this
# terminal as it happens: the kernel's log, every process started and ended, every
# launchd job, and the unified log at every level. The terminal is also the serial
# console, so a shell is there to log in to. Kept in build/qemu/debug.log.
#
#   Meta/run_debug.sh [Meta/run.sh's options] [-- extra QEMU arguments]
#
# With --gdb, QEMU also waits for a debugger on localhost:1234.

exec "$(dirname "${BASH_SOURCE[0]}")/run.sh" --debug "$@"
