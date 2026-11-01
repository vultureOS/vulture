/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

// Notifications by name, as macOS's notify(3): one process posts
// "com.vulture.example.changed", every process that registered for it
// learns of it. notifyd does the telling.

#define NOTIFY_STATUS_OK 0
#define NOTIFY_STATUS_INVALID_NAME 1
#define NOTIFY_STATUS_INVALID_TOKEN 2
#define NOTIFY_STATUS_INVALID_PORT 3
#define NOTIFY_STATUS_INVALID_FILE 4
#define NOTIFY_STATUS_INVALID_REQUEST 6
#define NOTIFY_STATUS_NOT_AUTHORIZED 7
#define NOTIFY_STATUS_FAILED 1000000

// notify_register_file_descriptor: use the descriptor already in *fd. Not
// supported: such a call fails with NOTIFY_STATUS_INVALID_REQUEST.
#define NOTIFY_REUSE 0x00000001

uint32_t notify_post(char const* name);

// A token notify_check() can ask about: 1 in *check if the name was
// posted since the last check (and on the first).
uint32_t notify_register_check(char const* name, int* out_token);
uint32_t notify_check(int token, int* check);

// A descriptor that becomes readable with each post: the token, 4 bytes,
// big-endian (a pipe's read end).
uint32_t notify_register_file_descriptor(char const* name, int* notify_fd, int flags, int* out_token);

// A port receive right (sys/port.h): each post sends it a message
// (NotifyClient.notification, see Notify.ipc).
uint32_t notify_register_port(char const* name, int* receive_right, int flags, int* out_token);

uint32_t notify_cancel(int token);
uint32_t notify_set_state(int token, uint64_t state);
uint32_t notify_get_state(int token, uint64_t* state);
uint32_t notify_is_valid_token(int token);

__END_DECLS
