/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/StringView.h>

// The boot word debugfault=KIND: a mitigation test has a program ask, through sysctl "debug.fault",
// for the kernel to make a mistake with memory of that program's, as only then is there any to
// make it with. Without the boot word, the sysctl isn't there.

namespace Kernel {

void set_debug_fault(StringView kind);
[[nodiscard]] StringView debug_fault();

}
