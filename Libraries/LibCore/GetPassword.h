/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/String.h>

namespace Core {

// Shows `prompt` and reads a line from the terminal (/dev/tty, or
// standard input without one) without echoing it, as macOS's
// readpassphrase does. Nothing (an error) at end of input.
ErrorOr<String> get_password(StringView prompt = "Password:"_sv);

}
