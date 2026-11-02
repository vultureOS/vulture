/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/StringView.h>

// A person's Desktop, Documents and Downloads are theirs: an application that didn't come with the
// system only gets in if they say so. The first time it tries, the attempt waits while privacyd asks
// them, or finds out what they said before; a "no" is EPERM.
//
// Which application is decided by the kernel, from what code was started: an installed program, and
// everything it goes on to start, even programs that came with the system. So it can't get around
// this by having /bin/cat do it, and can't claim to be someone else.
//
// With nobody to ask (no privacyd, or nobody logged in) the answer is no.

namespace Kernel::Privacy {

void initialize();

// Is the current process allowed `path`, canonical, as far as this goes?
[[nodiscard]] ErrorOr<void> check(StringView path);

}
