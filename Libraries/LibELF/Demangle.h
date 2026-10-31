/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Format.h>

namespace ELF {

// Writes a readable form of an Itanium C++ ABI mangled name: the qualified
// name without parameter types, e.g. "_ZN6Kernel5panicEN4Base10StringViewE"
// becomes "Kernel::panic". Simple template arguments are spelled out
// ("Base::Optional<int>::value"); anything more involved shows as "<...>".
//
// Returns false, writing nothing, if `mangled` is not a mangled name or uses
// a construct this demangler does not cover. Callers then show it as is.
bool demangle(StringView mangled, FormatSink& output);

}
