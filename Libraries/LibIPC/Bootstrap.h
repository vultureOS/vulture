/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibIPC/Port.h>
#include <LibBase/StringView.h>

// Finding services by name, through this process's bootstrap port (which
// vlaunchd serves), as with launchd's bootstrap_look_up and friends.

namespace IPC {

// A descriptor for the bootstrap port (ENOENT if the process has none).
ErrorOr<Port> bootstrap_port();
// A send right to the service `name`.
ErrorOr<Port> look_up(StringView name);
// For a service vlaunchd started: its receive right.
ErrorOr<Port> check_in(StringView name);
// Makes a service of our own findable by `name`.
ErrorOr<void> register_service(StringView name, Port send_right);

}
