/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// waitpid() options and status encoding, shared by the kernel and LibC.
// A status holds either an exit code (bits 8-15, low bits 0) or the number
// of the signal that killed the process (bits 0-6, bit 7 = core dumped).

#define WNOHANG 1
#define WUNTRACED 2

#define VULTURE_W_EXITED(code) (((code) & 0xff) << 8)
#define VULTURE_W_SIGNALED(signal, core) (((signal) & 0x7f) | ((core) ? 0x80 : 0))

#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WIFSIGNALED(status) (((status) & 0x7f) != 0 && ((status) & 0x7f) != 0x7f)
#define WTERMSIG(status) ((status) & 0x7f)
#define WCOREDUMP(status) ((status) & 0x80)
