/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/Vector.h>
#include <LibText/Typeface.h>

// The OpenType layout tables: GSUB, which turns glyphs into other glyphs, and GPOS, which says where they go. This is a machine for running what's in a font, and a font can say
// anything: everything it reads is checked, and everything it can make happen is limited in how much of it there can be.

namespace Text {

// A glyph on its way through, with what's known about where it came from.
struct ShapingGlyph {
    GlyphIndex glyph { 0 };
    u32 code_point { 0 };
    // Where in the text it came from. Glyphs that came from the same place stay together, and can't be told apart by selecting text.
    u32 cluster { 0 };
    u32 mask { 0 };

    // From GDEF, or worked out if it doesn't say.
    enum Class : u8 {
        Unclassified = 0,
        Base = 1,
        Ligature = 2,
        Mark = 3,
        Component = 4,
    };
    u8 glyph_class { Unclassified };
    u8 mark_attachment_class { 0 };

    // Which ligature it's part of, and for a mark on one, which part of that it's on (from 1).
    u8 ligature_id { 0 };
    u8 ligature_component { 0 };
    // How many parts it's made of, if it's a ligature that's been made here.
    u8 ligature_parts { 0 };

    // For the shaper of whichever script this is.
    u8 shaping_category { 0 };
    u8 shaping_position { 0 };
    u8 syllable { 0 };
    bool is_hidden { false }; // A default ignorable: takes up nothing, and isn't drawn.
    bool is_zwj { false };
    bool is_zwnj { false };
    bool was_substituted { false };
    bool is_multiplied { false };
    bool is_ligated { false };

    // In font units.
    i32 x_advance { 0 };
    i32 y_advance { 0 };
    i32 x_offset { 0 };
    i32 y_offset { 0 };

    // What it's attached to, as how many places before or after this it is: 0 for nothing.
    i32 attached_to { 0 };
    enum Attachment : u8 {
        NotAttached,
        Marked,
        Cursive,
    };
    u8 attachment { NotAttached };
};

using ShapingBuffer = Vector<ShapingGlyph>;

// What a run of text is being looked up as.
struct LayoutScript {
    // OpenType's tags for it, in the order to try: dev2, then deva, for instance. Then DFLT and latn.
    u32 tags[3] { 0, 0, 0 };
    u32 language { 0 }; // 0 is the default.
};

class OpenTypeLayout {
public:
    enum class Table : u8 {
        GSUB,
        GPOS,
    };

    OpenTypeLayout(Typeface const& typeface, Table which);

    [[nodiscard]] bool exists() const { return m_table != 0; }

    // Which script and language this font has that's nearest to what's wanted. Features are then looked for there. False if it hasn't anything for that script: DFLT is then what's used.
    bool select_script(LayoutScript const&);
    [[nodiscard]] u32 selected_script_tag() const { return m_script_tag; }

    // The lookups of a feature, in the current script and language, added to what's there already.
    void collect_lookups(u32 feature, Vector<u16>& lookups) const;
    [[nodiscard]] bool has_feature(u32 feature) const;
    // The feature that always applies in this language, if there is one.
    [[nodiscard]] Optional<u16> required_feature_index() const { return m_required_feature; }
    void collect_lookups_of_feature_index(u16 feature_index, Vector<u16>& lookups) const;

    // One lookup, over everything in the buffer whose mask has this in it.
    void apply_lookup(u16 lookup_index, ShapingBuffer&, u32 mask, bool is_right_to_left, bool joiners_count = false) const;

    // The GDEF table's say, on every glyph.
    void classify(ShapingBuffer&) const;
    [[nodiscard]] bool has_glyph_classes() const;

private:
    struct Run;

    [[nodiscard]] u16 u16_at(size_t at) const { return m_typeface.layout_u16(at); }
    [[nodiscard]] i16 i16_at(size_t at) const { return static_cast<i16>(u16_at(at)); }
    [[nodiscard]] u32 u32_at(size_t at) const { return m_typeface.layout_u32(at); }

    [[nodiscard]] int coverage_index(size_t coverage, GlyphIndex) const;
    [[nodiscard]] u16 class_of(size_t definition, GlyphIndex) const;
    [[nodiscard]] bool is_in_mark_set(u16 set, GlyphIndex) const;

    [[nodiscard]] bool apply_lookup_at(Run&, u16 lookup_index) const;
    [[nodiscard]] bool apply_subtable(Run&, u16 type, size_t subtable) const;
    [[nodiscard]] bool apply_gsub_subtable(Run&, u16 type, size_t subtable) const;
    [[nodiscard]] bool apply_gpos_subtable(Run&, u16 type, size_t subtable) const;
    [[nodiscard]] bool apply_context(Run&, size_t subtable) const;
    [[nodiscard]] bool apply_chain_context(Run&, size_t subtable) const;

    void apply_value_record(ShapingGlyph&, size_t table, size_t record, u16 format) const;
    [[nodiscard]] size_t value_record_size(u16 format) const;

    Typeface const& m_typeface;
    Table m_which;
    size_t m_table { 0 };
    size_t m_gdef { 0 };
    size_t m_script_list { 0 };
    size_t m_feature_list { 0 };
    size_t m_lookup_list { 0 };
    size_t m_language_system { 0 };
    u32 m_script_tag { 0 };
    Optional<u16> m_required_feature;
};

}
