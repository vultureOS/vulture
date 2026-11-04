/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

// `fd` is anything open on the volume. These are for root. Going back takes effect when the volume is next mounted,
// and leaves the snapshot there to go back to again; those made after it go.
int fs_snapshot_create(int fd, char const* name, unsigned int flags);
int fs_snapshot_delete(int fd, char const* name, unsigned int flags);
int fs_snapshot_revert(int fd, char const* name, unsigned int flags);
// Their names, each with a zero after it: how many bytes that is. (A Mac has a more elaborate way of asking.)
ssize_t fs_snapshot_list(int fd, char* names, size_t size, unsigned int flags);

__END_DECLS
