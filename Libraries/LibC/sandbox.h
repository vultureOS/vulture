/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>
#include <sys/types.h>

// Giving up what this process, and everything it starts, doesn't need: see Kernel/Security/Sandbox.h.

__BEGIN_DECLS

// `profile` names a file in /System/Library/Sandbox/Profiles.
#define SANDBOX_NAMED 1

// `profile` is its text, or with SANDBOX_NAMED its name. 0, or -1 and errno, with what went wrong
// in *error (for sandbox_free_error), if `error` isn't null.
int sandbox_init(char const* profile, unsigned long long flags, char** error);
// The same, for a process about to run `program`, which is what the ${PROGRAM} in a profile means.
int sandbox_init_for_program(char const* profile, unsigned long long flags, char const* program, char** error);
void sandbox_free_error(char*);

// Whether that process's sandbox lets it look up that service: 0, or -1 and errno.
int sandbox_check_service(pid_t, char const* service);

// Outside a sandbox: a token that lets whoever redeems it reach this path, as an Open panel's choice does. malloc()ed, or null
// and errno.
char* sandbox_extension_issue(char const* path, int writable);
// Inside one: from now on, that path can be reached. 0, or -1 and errno.
int sandbox_extension_consume(char const* token);
// Whether this process is in a sandbox at all: 1 or 0.
int sandbox_is_active(void);

__END_DECLS
