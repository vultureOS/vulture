/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Security/DebugFault.h>

namespace Kernel {

static StringView s_kind;

void set_debug_fault(StringView kind)
{
    s_kind = kind;
}

StringView debug_fault()
{
    return s_kind;
}

}
