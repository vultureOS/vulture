/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/port.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

// Ports (see Kernel/API/POSIX/port.h): -1 and errno on failure.

// A new port: fds[0] its receive right, fds[1] a send right. `flags`:
// O_CLOEXEC.
int port_create(int fds[2], int flags);
// Another send right, from the receive right.
int port_make_send(int receive_right, int flags);
int port_send(int send_right, struct vulture_port_message const*, int flags);
// EMSGSIZE (with `size` set, the message still queued) if it does not fit.
int port_receive(int receive_right, struct vulture_port_received*, int flags);
// Sends and waits for the answer (the receiver gets a reply right).
int port_call(int send_right, struct vulture_port_message const*, struct vulture_port_received* answer, int flags);

// A new descriptor for this process's bootstrap port (the name service,
// vlaunchd), or -1 and ENOENT if it has none.
int bootstrap_port_get(int flags);
// `send_right` becomes the bootstrap port, for this process and those it
// starts.
int bootstrap_port_set(int send_right);

__END_DECLS
