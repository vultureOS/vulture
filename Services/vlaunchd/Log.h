/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace Launchd {

// Where messages go: a file, in syslog's form ("Nov  1 12:34:56 vulture
// vlaunchd[1]: ..."), as macOS's /var/log/system.log has them; standard
// error if it cannot be opened.
void open_log(char const* path);
[[gnu::format(printf, 1, 2)]] void log(char const* format, ...);

}
