/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/RefPtr.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>
#include <LibText/Typeface.h>
#include <LibUnicode/Unicode.h>

// From text to runs a shaper can take: which way each part goes, which script it's in, and which font has it.

namespace Text {

// Which fonts to go to, when the one that was asked for hasn't got what's wanted. They're opened the first time they're needed, as most text never needs most of them.
class FontCascade {
public:
    [[nodiscard]] static FontCascade& the();

    // Where the font files are: /System/Library/Fonts, unless something else is being tested.
    void set_directory(StringView);

    // The first font, starting from this one, that has this character. Null if nothing has.
    [[nodiscard]] Typeface const* typeface_for(Typeface const& primary, Unicode::CodePoint, Unicode::Script);
    // The font of pictures, if it has one for this.
    [[nodiscard]] Typeface const* emoji_typeface_for(Unicode::CodePoint);

private:
    FontCascade() = default;

    struct Loaded {
        String file;
        RefPtr<Typeface> typeface;
        bool tried { false };
    };

    [[nodiscard]] Typeface const* load(StringView file);

    String m_directory;
    Vector<Loaded> m_loaded;
};

struct TextItem {
    // In code points.
    size_t start { 0 };
    size_t end { 0 };
    Unicode::Script script { Unicode::Script::Latin };
    bool is_right_to_left { false };
    Typeface const* typeface { nullptr };
};

// In the order to draw them, left to right. `primary` is what's used wherever it can be.
[[nodiscard]] ErrorOr<Vector<TextItem>> itemize(Span<Unicode::CodePoint const> text, Typeface const& primary, Unicode::ParagraphDirection = Unicode::ParagraphDirection::Auto);

}
