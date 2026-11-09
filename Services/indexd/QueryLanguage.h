/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibDatabase/Value.h>

// The language of mdfind: `kMDItemFSSize > 1000 && kMDItemFSName == "*.txt"c`, into SQL over the table of items.

namespace Index {

struct TranslatedQuery {
    String where;
    Vector<Database::Value> parameters;
};

// Plain words aren't this language: it's for what has an operator in it.
[[nodiscard]] bool looks_like_query_language(StringView);
ErrorOr<TranslatedQuery> translate_query(StringView, String& message);

// The columns of the table of items, and how mdls and queries know them.
struct Attribute {
    StringView name;
    StringView column;
    bool is_date;
};
[[nodiscard]] Span<Attribute const> known_attributes();

}
