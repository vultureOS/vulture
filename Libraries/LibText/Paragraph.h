/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>
#include <LibText/Font.h>
#include <LibUnicode/Unicode.h>

// A paragraph with more than one style in it, laid out: where its lines end, by Unicode's rules; which way each part of each line goes; which glyphs, in which fonts, and where.
// All in points, so it's the same at any scale.

namespace Text {

// Part of the text (in bytes), and which of the styles it's in.
struct StyleRange {
    size_t start { 0 };
    size_t end { 0 };
    u32 style { 0 };
};

struct ParagraphStyle {
    Typeface const* typeface { nullptr };
    float points { 13 };
};

enum class ParagraphAlignment : u8 {
    // Whichever side the paragraph starts from.
    Natural,
    Left,
    Center,
    Right,
};

class ParagraphLayout {
public:
    struct Run {
        u32 style { 0 };
        // Glyphs here are from the start of the run, in points.
        Vector<PositionedGlyph> glyphs;
        float x { 0 };
        float width { 0 };
        bool is_right_to_left { false };
    };
    struct Line {
        size_t start { 0 };
        size_t end { 0 };
        // Its baseline, from the top of the paragraph, and how far it goes up and down from there.
        float baseline { 0 };
        float ascent { 0 };
        float descent { 0 };
        float line_gap { 0 };
        float x { 0 };
        float width { 0 };
        Vector<Run> runs;

        [[nodiscard]] float top() const { return baseline - ascent; }
        [[nodiscard]] float height() const { return ascent + descent + line_gap; }
    };

    // `width` of 0 or less is no wrapping. `ranges` cover the text in order, with nothing between them.
    [[nodiscard]] static ErrorOr<ParagraphLayout> create(StringView text, Span<StyleRange const>, Span<ParagraphStyle const>, float width, ParagraphAlignment = ParagraphAlignment::Natural, Unicode::ParagraphDirection = Unicode::ParagraphDirection::Auto);

    [[nodiscard]] Span<Line const> lines() const { return m_lines.span(); }
    [[nodiscard]] float height() const { return m_height; }
    [[nodiscard]] float widest_line() const { return m_widest; }
    [[nodiscard]] bool is_right_to_left() const { return m_is_right_to_left; }

    // Which line this place in the text is on.
    [[nodiscard]] size_t line_of(size_t offset) const;
    // Where a caret goes for this place in the text.
    [[nodiscard]] float caret_x(size_t offset) const;
    // The place in the text nearest this point.
    [[nodiscard]] size_t offset_at(float x, float y) const;
    // The rectangles a selection of this much of the text covers, on each line: more than one on a line if its direction changes.
    struct Span1D {
        float left;
        float right;
        size_t line;
    };
    [[nodiscard]] Vector<Span1D> selection_spans(size_t start, size_t end) const;

private:
    Vector<Line> m_lines;
    size_t m_text_length { 0 };
    float m_height { 0 };
    float m_widest { 0 };
    bool m_is_right_to_left { false };
};

}
