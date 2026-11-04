/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibBase/Vector.h>
#include <LibText/OpenTypeLayout.h>
#include <LibText/Typeface.h>
#include <LibUnicode/Unicode.h>

// From text to glyphs: what a font does to a run of one script, in one direction. Finding the runs, and which font each one's in, is someone else's job (Itemizer.h).

namespace Text {

struct ShapedGlyph {
    GlyphIndex glyph { 0 };
    // Where in the text it came from.
    u32 cluster { 0 };
    // In font units.
    i32 x_advance { 0 };
    i32 y_advance { 0 };
    i32 x_offset { 0 };
    i32 y_offset { 0 };
};

struct ShapeRequest {
    Span<Unicode::CodePoint const> text;
    Unicode::Script script { Unicode::Script::Latin };
    bool is_right_to_left { false };
    // OpenType's tag for the language, if it's known: 0 otherwise.
    u32 language { 0 };
};

// In the order they're to be drawn: left to right on the page, however the text reads.
[[nodiscard]] ErrorOr<Vector<ShapedGlyph>> shape(Typeface const&, ShapeRequest const&);

namespace Shaping {

// What it's all put together with: which features to apply, in which order, to which glyphs. A pause is for whatever has to happen between one set of features and the next.
class Plan {
public:
    Plan(OpenTypeLayout const& gsub, OpenTypeLayout const& gpos);

    // For everything: bit 0.
    static constexpr u32 global_mask = 1;

    void add_global(u32 feature) { add(feature, global_mask); }
    // A bit of its own, to be set only on the glyphs it's for. If it's asked for a second time, it's the same bit.
    [[nodiscard]] u32 mask_for(u32 feature);
    // If `joiners_count`, a zero-width joiner isn't looked straight through: it stops what would otherwise match, which is what it's for in a script where it decides what forms are made.
    void add(u32 feature, u32 mask, bool joiners_count = false);
    void pause(Function<void(ShapingBuffer&)> = nullptr);

    void substitute(ShapingBuffer&, bool is_right_to_left) const;
    void position(ShapingBuffer&, bool is_right_to_left) const;
    void add_position(u32 feature);

private:
    struct Stage {
        Vector<u32> features;
        Vector<u32> masks;
        Vector<bool> joiners_count;
        Function<void(ShapingBuffer&)> after;
    };

    OpenTypeLayout const& m_gsub;
    OpenTypeLayout const& m_gpos;
    Vector<Stage> m_stages;
    Vector<u32> m_position_features;
    Vector<u32> m_named_features;
    Vector<u32> m_named_masks;
    u32 m_next_bit { 2 };
};

// A script's own part in it, as far as there is one.
void prepare_arabic(ShapingBuffer&, Plan&, bool is_syriac);
void prepare_indic(Typeface const&, OpenTypeLayout const& gsub, ShapingBuffer&, Plan&, Unicode::Script);
void prepare_hangul(ShapingBuffer&, Plan&);

}

}
