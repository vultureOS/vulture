/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// libSystem's own __dso_handle (each program has one from crt0).
.data
.balign 8
.global __dso_handle
.hidden __dso_handle
__dso_handle:
    .quad __dso_handle
