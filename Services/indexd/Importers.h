/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Optional.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

// What can be said of a file by looking inside it. Each importer reads what it needs and no more; they all assume that what
// they're reading could be anything.

namespace Index {

struct Metadata {
    String content_type;
    String kind_description;
    // What a file's about, for finding it. Folded, and each one once.
    Vector<String> words;
    Optional<i64> pixel_width;
    Optional<i64> pixel_height;
    String version;
};

// Lower case, without accents: what's compared.
[[nodiscard]] String fold(StringView);
// The words of a text, folded; and a name's as well as what's joined up in it (TextEditor, as text and editor).
void words_of(StringView text, Vector<String>& out, size_t most);
void words_of_name(StringView name, Vector<String>& out);

void import_file(StringView path, Metadata&);

}
