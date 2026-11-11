/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Color.h>

// A file's tags, which go with it wherever it goes: in the extended attribute a Mac keeps them in, as a list of names.

namespace Desktop {

struct TagColor {
    StringView name;
    Gfx::Color color;
};

// The seven there are to begin with, in Finder's order.
[[nodiscard]] Span<TagColor const> standard_tags();
[[nodiscard]] Optional<Gfx::Color> color_of_tag(StringView name);

[[nodiscard]] Vector<String> tags_of(StringView path);
ErrorOr<void> set_tags(StringView path, Span<String const> tags);
// On if it's off, off if it's on.
ErrorOr<void> toggle_tag(StringView path, StringView tag);

}
