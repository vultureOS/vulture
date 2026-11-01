#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The text console, with no desktop: a log-in prompt in QEMU's window, and in this
# terminal over the serial line.
#
#   Meta/run_console.sh [Meta/run.sh's options] [-- extra QEMU arguments]

exec "$(dirname "${BASH_SOURCE[0]}")/run.sh" --console "$@"
