/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibJSON/JSON.h>

// Property lists, as a Mac keeps them: the XML form, the JSON one this system's own files use, and reading the binary one.
// In memory they're JSON values: dictionaries are objects and arrays arrays. A <date> or <data> comes back as a string, and
// goes out as a string again: for what's here, nothing has needed more.

namespace JSON {

// Whichever form it's in.
[[nodiscard]] ErrorOr<Value> parse_property_list(ReadonlyBytes);
[[nodiscard]] ErrorOr<String> serialize_property_list_xml(Value const&);
// As `defaults read` shows them: { key = value; }.
[[nodiscard]] ErrorOr<String> describe_property_list(Value const&);
// As `plutil -p` shows them.
[[nodiscard]] ErrorOr<String> print_property_list_for_people(Value const&);
// The binary form a Mac uses for its own: bplist00.
[[nodiscard]] ErrorOr<Vector<u8>> serialize_property_list_binary(Value const&);

}
