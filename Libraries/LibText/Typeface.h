/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/HashMap.h>
#include <LibBase/RefPtr.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Path.h>

// An OpenType or TrueType font file: its glyphs' outlines (glyf or CFF), metrics,
// character map, kerning (GPOS, or the old kern table) and substitutions (GSUB).
// Font files come from anywhere: nothing here trusts what they say.

namespace Text {

using GlyphIndex = u16;

// An OpenType feature's four letters.
[[nodiscard]] constexpr u32 feature_tag(char const (&name)[5])
{
    return (static_cast<u32>(static_cast<u8>(name[0])) << 24) | (static_cast<u32>(static_cast<u8>(name[1])) << 16) | (static_cast<u32>(static_cast<u8>(name[2])) << 8) | static_cast<u8>(name[3]);
}

class Typeface final : public RefCounted<Typeface> {
public:
    // Keeps the bytes.
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Typeface>> parse(Vector<u8> data);

    [[nodiscard]] u16 units_per_em() const { return m_units_per_em; }
    [[nodiscard]] i16 ascent() const { return m_ascent; }
    [[nodiscard]] i16 descent() const { return m_descent; } // Negative.
    [[nodiscard]] i16 line_gap() const { return m_line_gap; }
    [[nodiscard]] i16 cap_height() const { return m_cap_height; }
    [[nodiscard]] i16 x_height() const { return m_x_height; }
    [[nodiscard]] u16 weight() const { return m_weight; }
    [[nodiscard]] u16 glyph_count() const { return m_glyph_count; }
    [[nodiscard]] String const& family_name() const { return m_family_name; }
    [[nodiscard]] String const& style_name() const { return m_style_name; }
    // A small number telling typefaces apart (for glyph caches).
    [[nodiscard]] u32 id() const { return m_id; }

    // 0 (.notdef) if the font has no glyph for it.
    [[nodiscard]] GlyphIndex glyph_for_code_point(u32) const;
    [[nodiscard]] u16 advance_of(GlyphIndex) const;
    // In font units, y up.
    [[nodiscard]] Gfx::Path outline_of(GlyphIndex) const;
    // How much to add to the advance of `left` before `right`.
    [[nodiscard]] i16 kerning(GlyphIndex left, GlyphIndex right) const;
    // Applies the GSUB features listed, in the font's order of lookups.
    void substitute(Vector<GlyphIndex>& glyphs, Span<u32 const> features) const;

private:
    Typeface() = default;

    struct Table {
        size_t offset { 0 };
        size_t size { 0 };
        [[nodiscard]] bool exists() const { return size != 0; }
    };

    [[nodiscard]] u8 byte_at(size_t) const;
    [[nodiscard]] u16 u16_at(size_t) const;
    [[nodiscard]] i16 i16_at(size_t offset) const { return static_cast<i16>(u16_at(offset)); }
    [[nodiscard]] u32 u32_at(size_t) const;

    ErrorOr<void> read_directory();
    ErrorOr<void> read_metrics();
    void read_names();
    void read_character_map();
    void read_cff();
    void read_layout_features(Table const& table, Vector<u32>& tags, Vector<Vector<u16>>& lookups) const;

    void append_truetype_outline(GlyphIndex, Gfx::Path&, Gfx::AffineTransform const&, int depth) const;
    void append_simple_outline(size_t offset, size_t end, i16 contours, Gfx::Path&, Gfx::AffineTransform const&) const;
    void append_cff_outline(GlyphIndex, Gfx::Path&) const;

    [[nodiscard]] int coverage_index(size_t coverage_offset, GlyphIndex) const;
    [[nodiscard]] u16 class_of(size_t class_definition_offset, GlyphIndex) const;
    [[nodiscard]] i16 gpos_kerning(GlyphIndex left, GlyphIndex right) const;
    [[nodiscard]] i16 pair_adjustment(size_t subtable, GlyphIndex left, GlyphIndex right, bool& found) const;
    [[nodiscard]] i16 kern_table_kerning(GlyphIndex left, GlyphIndex right) const;
    void apply_lookup(u16 lookup_index, Vector<GlyphIndex>&) const;
    bool apply_subtable(u16 type, size_t subtable, Vector<GlyphIndex>&, size_t& position) const;
    bool apply_single_at(u16 lookup_index, Vector<GlyphIndex>&, size_t position) const;
    bool apply_single_at_inner(u16 lookup_index, Vector<GlyphIndex>&, size_t position) const;

    Vector<u8> m_data;
    u32 m_id { 0 };
    Table m_head, m_hhea, m_hmtx, m_maxp, m_cmap, m_loca, m_glyf, m_kern, m_gpos, m_gsub, m_os2, m_name, m_cff;
    u16 m_units_per_em { 1000 };
    i16 m_ascent { 0 };
    i16 m_descent { 0 };
    i16 m_line_gap { 0 };
    i16 m_cap_height { 0 };
    i16 m_x_height { 0 };
    u16 m_weight { 400 };
    u16 m_glyph_count { 0 };
    u16 m_metric_count { 0 };
    bool m_long_loca { false };
    String m_family_name;
    String m_style_name;

    // cmap: the subtable used, and its format.
    size_t m_cmap_subtable { 0 };
    u16 m_cmap_format { 0 };

    // CFF.
    struct CFFIndex {
        size_t data { 0 }; // Where object 1 would start, less its offset.
        size_t offsets { 0 };
        u32 count { 0 };
        u8 offset_size { 0 };
        size_t end { 0 };
    };
    [[nodiscard]] CFFIndex read_cff_index(size_t at) const;
    [[nodiscard]] bool cff_object(CFFIndex const&, u32 index, size_t& start, size_t& end) const;
    void run_charstring(size_t start, size_t end, struct CharstringState&, CFFIndex const& local_subroutines, int depth) const;
    CFFIndex m_cff_charstrings;
    CFFIndex m_cff_global_subroutines;
    CFFIndex m_cff_local_subroutines;

    Vector<u32> m_gpos_features;
    Vector<Vector<u16>> m_gpos_feature_lookups;
    Vector<u16> m_kerning_lookups;
    Vector<u32> m_gsub_features;
    Vector<Vector<u16>> m_gsub_feature_lookups;
    mutable HashMap<u32, i16> m_kerning_cache;
    mutable int m_substitution_depth { 0 };
};

}
