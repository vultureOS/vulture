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

// Booted with "debug": messages go to the console as well, and those below are made.
void set_debug_logging(bool);
[[nodiscard]] bool debug_logging();
// What is too ordinary for the log otherwise: jobs starting and ending as they should.
[[gnu::format(printf, 1, 2)]] void debug(char const* format, ...);

}
