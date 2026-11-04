/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Atomic.h>
#include <LibBase/Checksum.h>
#include <LibBase/New.h>
#include <LibBase/QuickSort.h>
#include <LibText/Typeface.h>

namespace Text {

namespace {

constexpr u32 tag(char const (&name)[5])
{
    return feature_tag(name);
}

Atomic<u32> s_next_id { 1 };

}

u8 Typeface::byte_at(size_t offset) const
{
    return offset < m_data.size() ? m_data[offset] : 0;
}

u16 Typeface::u16_at(size_t offset) const
{
    if (offset + 2 > m_data.size())
        return 0;
    return static_cast<u16>((m_data[offset] << 8) | m_data[offset + 1]);
}

u32 Typeface::u32_at(size_t offset) const
{
    if (offset + 4 > m_data.size())
        return 0;
    return (static_cast<u32>(m_data[offset]) << 24) | (static_cast<u32>(m_data[offset + 1]) << 16) | (static_cast<u32>(m_data[offset + 2]) << 8) | m_data[offset + 3];
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::parse(Vector<u8> data)
{
    auto* typeface = new (Base::nothrow) Typeface;
    if (!typeface)
        return Error::from_string_literal("out of memory");
    auto reference = adopt_ref(*typeface);
    typeface->m_owned_data = move(data);
    typeface->m_data = typeface->m_owned_data.span();
    TRY(typeface->read_everything());
    return reference;
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::parse_mapped(ReadonlyBytes data)
{
    auto* typeface = new (Base::nothrow) Typeface;
    if (!typeface)
        return Error::from_string_literal("out of memory");
    auto reference = adopt_ref(*typeface);
    typeface->m_data = data;
    TRY(typeface->read_everything());
    return reference;
}

ErrorOr<void> Typeface::read_everything()
{
    auto* typeface = this;
    typeface->m_id = s_next_id.fetch_add(1);
    TRY(typeface->read_directory());
    TRY(typeface->read_metrics());
    typeface->read_names();
    typeface->read_character_map();
    typeface->read_variations();
    if (typeface->m_cff.exists())
        typeface->read_cff();
    typeface->read_layout_features(typeface->m_gpos, typeface->m_gpos_features, typeface->m_gpos_feature_lookups);
    typeface->read_layout_features(typeface->m_gsub, typeface->m_gsub_features, typeface->m_gsub_feature_lookups);
    for (size_t i = 0; i < typeface->m_gpos_features.size(); ++i) {
        if (typeface->m_gpos_features[i] == tag("kern")) {
            for (u16 lookup : typeface->m_gpos_feature_lookups[i]) {
                if (!typeface->m_kerning_lookups.contains_slow(lookup))
                    typeface->m_kerning_lookups.append(lookup);
            }
        }
    }
    quick_sort(typeface->m_kerning_lookups.span());
    return {};
}

ErrorOr<void> Typeface::read_directory()
{
    if (m_data.size() < 12)
        return Error::from_string_literal("font: too short");
    u32 version = u32_at(0);
    // A collection: the first of what's in it. (Which language's forms are wanted is for `locl`.)
    if (version == tag("ttcf")) {
        if (u32_at(8) == 0)
            return Error::from_string_literal("font: an empty collection");
        m_directory_offset = u32_at(12);
        version = u32_at(m_directory_offset);
    }
    if (version != 0x00010000 && version != tag("OTTO") && version != tag("true"))
        return Error::from_string_literal("font: not an OpenType file");
    u16 count = u16_at(m_directory_offset + 4);
    for (u16 i = 0; i < count; ++i) {
        size_t record = m_directory_offset + 12 + static_cast<size_t>(i) * 16;
        u32 name = u32_at(record);
        size_t offset = u32_at(record + 8);
        size_t size = u32_at(record + 12);
        if (offset > m_data.size() || size > m_data.size() - offset)
            continue;
        Table table { offset, size };
        switch (name) {
        case tag("head"):
            m_head = table;
            break;
        case tag("hhea"):
            m_hhea = table;
            break;
        case tag("hmtx"):
            m_hmtx = table;
            break;
        case tag("maxp"):
            m_maxp = table;
            break;
        case tag("cmap"):
            m_cmap = table;
            break;
        case tag("loca"):
            m_loca = table;
            break;
        case tag("glyf"):
            m_glyf = table;
            break;
        case tag("kern"):
            m_kern = table;
            break;
        case tag("GPOS"):
            m_gpos = table;
            break;
        case tag("GSUB"):
            m_gsub = table;
            break;
        case tag("OS/2"):
            m_os2 = table;
            break;
        case tag("name"):
            m_name = table;
            break;
        case tag("CFF "):
            m_cff = table;
            break;
        case tag("fvar"):
            m_fvar = table;
            break;
        case tag("avar"):
            m_avar = table;
            break;
        case tag("gvar"):
            m_gvar = table;
            break;
        case tag("COLR"):
            m_colr = table;
            break;
        case tag("CPAL"):
            m_cpal = table;
            break;
        case tag("CBDT"):
            m_cbdt = table;
            break;
        case tag("CBLC"):
            m_cblc = table;
            break;
        default:
            break;
        }
    }
    if (!m_head.exists() || !m_hhea.exists() || !m_hmtx.exists() || !m_maxp.exists() || !m_cmap.exists())
        return Error::from_string_literal("font: a required table is missing");
    // (A font of pictures has nothing else.)
    if (!m_cff.exists() && (!m_glyf.exists() || !m_loca.exists()) && !(m_cbdt.exists() && m_cblc.exists()))
        return Error::from_string_literal("font: no outlines");
    return {};
}

ErrorOr<void> Typeface::read_metrics()
{
    m_units_per_em = u16_at(m_head.offset + 18);
    if (m_units_per_em == 0)
        return Error::from_string_literal("font: no units per em");
    m_long_loca = i16_at(m_head.offset + 50) != 0;
    m_glyph_count = u16_at(m_maxp.offset + 4);
    m_ascent = i16_at(m_hhea.offset + 4);
    m_descent = i16_at(m_hhea.offset + 6);
    m_line_gap = i16_at(m_hhea.offset + 8);
    m_metric_count = u16_at(m_hhea.offset + 34);
    if (m_metric_count == 0 || static_cast<size_t>(m_metric_count) * 4 > m_hmtx.size)
        return Error::from_string_literal("font: no glyph advances");
    if (m_os2.exists()) {
        m_weight = u16_at(m_os2.offset + 4);
        if (u16_at(m_os2.offset) >= 2 && m_os2.size >= 90) {
            m_x_height = i16_at(m_os2.offset + 86);
            m_cap_height = i16_at(m_os2.offset + 88);
        }
    }
    return {};
}

void Typeface::read_names()
{
    if (!m_name.exists())
        return;
    u16 count = u16_at(m_name.offset + 2);
    size_t strings = m_name.offset + u16_at(m_name.offset + 4);
    // The typographic names (16, 17) over the plain ones (1, 2).
    auto read = [&](u16 wanted) -> Optional<String> {
        for (u16 i = 0; i < count; ++i) {
            size_t record = m_name.offset + 6 + static_cast<size_t>(i) * 12;
            if (u16_at(record) != 3 || u16_at(record + 6) != wanted)
                continue;
            size_t length = u16_at(record + 8);
            size_t at = strings + u16_at(record + 10);
            if (at + length > m_data.size())
                continue;
            StringBuilder text;
            // UTF-16, of which the names here are in Basic Latin.
            for (size_t c = 0; c + 1 < length; c += 2) {
                if (byte_at(at + c) == 0)
                    text.append(static_cast<char>(byte_at(at + c + 1)));
            }
            auto result = text.to_string();
            if (!result.is_error())
                return result.release_value();
        }
        return {};
    };
    auto family = read(16);
    if (!family.has_value())
        family = read(1);
    if (family.has_value())
        m_family_name = family.release_value();
    auto style = read(17);
    if (!style.has_value())
        style = read(2);
    if (style.has_value())
        m_style_name = style.release_value();
}

void Typeface::read_character_map()
{
    u16 count = u16_at(m_cmap.offset + 2);
    int best_score = -1;
    for (u16 i = 0; i < count; ++i) {
        size_t record = m_cmap.offset + 4 + static_cast<size_t>(i) * 8;
        u16 platform = u16_at(record);
        u16 encoding = u16_at(record + 2);
        size_t subtable = m_cmap.offset + u32_at(record + 4);
        u16 format = u16_at(subtable);
        if (format == 14 && platform == 0 && encoding == 5)
            m_cmap_variations = subtable;
        int score = -1;
        // The whole of Unicode if we can have it.
        if (format == 12 && ((platform == 3 && encoding == 10) || platform == 0))
            score = 3;
        else if (format == 4 && ((platform == 3 && encoding == 1) || platform == 0))
            score = 2;
        if (score > best_score) {
            best_score = score;
            m_cmap_subtable = subtable;
            m_cmap_format = format;
        }
    }
}

GlyphIndex Typeface::glyph_for_code_point(u32 code_point) const
{
    if (m_cmap_format == 12) {
        u32 groups = u32_at(m_cmap_subtable + 12);
        size_t first = m_cmap_subtable + 16;
        if (groups > m_data.size() / 12)
            return 0;
        u32 low = 0, high = groups;
        while (low < high) {
            u32 middle = low + (high - low) / 2;
            size_t group = first + static_cast<size_t>(middle) * 12;
            u32 start = u32_at(group);
            u32 end = u32_at(group + 4);
            if (code_point < start) {
                high = middle;
            } else if (code_point > end) {
                low = middle + 1;
            } else {
                u32 glyph = u32_at(group + 8) + (code_point - start);
                return glyph < m_glyph_count ? static_cast<GlyphIndex>(glyph) : 0;
            }
        }
        return 0;
    }
    if (m_cmap_format == 4) {
        if (code_point > 0xffff)
            return 0;
        u16 segments = u16_at(m_cmap_subtable + 6) / 2;
        size_t ends = m_cmap_subtable + 14;
        size_t starts = ends + static_cast<size_t>(segments) * 2 + 2;
        size_t deltas = starts + static_cast<size_t>(segments) * 2;
        size_t ranges = deltas + static_cast<size_t>(segments) * 2;
        u32 low = 0, high = segments;
        while (low < high) {
            u32 middle = low + (high - low) / 2;
            if (code_point > u16_at(ends + static_cast<size_t>(middle) * 2))
                low = middle + 1;
            else
                high = middle;
        }
        if (low >= segments)
            return 0;
        size_t index = low;
        u16 start = u16_at(starts + index * 2);
        if (code_point < start)
            return 0;
        u16 delta = u16_at(deltas + index * 2);
        u16 range = u16_at(ranges + index * 2);
        u16 glyph;
        if (range == 0) {
            glyph = static_cast<u16>(code_point + delta);
        } else {
            glyph = u16_at(ranges + index * 2 + range + (code_point - start) * 2);
            if (glyph != 0)
                glyph = static_cast<u16>(glyph + delta);
        }
        return glyph < m_glyph_count ? glyph : 0;
    }
    return 0;
}

GlyphIndex Typeface::glyph_for_variation(u32 code_point, u32 selector) const
{
    if (m_cmap_variations == 0)
        return 0;
    auto const u24_at = [this](size_t at) { return (static_cast<u32>(byte_at(at)) << 16) | (static_cast<u32>(byte_at(at + 1)) << 8) | byte_at(at + 2); };
    u32 const records = u32_at(m_cmap_variations + 6);
    if (records > m_data.size() / 11)
        return 0;
    for (u32 i = 0; i < records; ++i) {
        size_t const record = m_cmap_variations + 10 + static_cast<size_t>(i) * 11;
        if (u24_at(record) != selector)
            continue;
        // Those that aren't the character's usual glyph are listed; those that are, are listed differently.
        size_t const non_default = u32_at(record + 7);
        if (non_default != 0) {
            size_t const table = m_cmap_variations + non_default;
            u32 const mappings = u32_at(table);
            if (mappings > m_data.size() / 5)
                return 0;
            u32 low = 0, high = mappings;
            while (low < high) {
                u32 const middle = low + (high - low) / 2;
                size_t const mapping = table + 4 + static_cast<size_t>(middle) * 5;
                u32 const unicode = u24_at(mapping);
                if (unicode == code_point) {
                    u16 const glyph = u16_at(mapping + 3);
                    return glyph < m_glyph_count ? glyph : 0;
                }
                if (unicode < code_point)
                    low = middle + 1;
                else
                    high = middle;
            }
        }
        size_t const default_ranges = u32_at(record + 3);
        if (default_ranges != 0) {
            size_t const table = m_cmap_variations + default_ranges;
            u32 const ranges = u32_at(table);
            if (ranges > m_data.size() / 4)
                return 0;
            for (u32 r = 0; r < ranges; ++r) {
                size_t const range = table + 4 + static_cast<size_t>(r) * 4;
                u32 const first = u24_at(range);
                if (code_point >= first && code_point <= first + byte_at(range + 3))
                    return glyph_for_code_point(code_point);
            }
        }
        return 0;
    }
    return 0;
}

size_t Typeface::offset_of_table(u32 wanted) const
{
    u16 const count = u16_at(m_directory_offset + 4);
    for (u16 i = 0; i < count; ++i) {
        size_t const record = m_directory_offset + 12 + static_cast<size_t>(i) * 16;
        if (u32_at(record) != wanted)
            continue;
        size_t const offset = u32_at(record + 8);
        size_t const size = u32_at(record + 12);
        return offset <= m_data.size() && size <= m_data.size() - offset ? offset : 0;
    }
    return 0;
}

size_t Typeface::size_of_table(u32 wanted) const
{
    u16 const count = u16_at(m_directory_offset + 4);
    for (u16 i = 0; i < count; ++i) {
        size_t const record = m_directory_offset + 12 + static_cast<size_t>(i) * 16;
        if (u32_at(record) == wanted)
            return u32_at(record + 12);
    }
    return 0;
}

u16 Typeface::advance_of(GlyphIndex glyph) const
{
    u16 index = glyph < m_metric_count ? glyph : static_cast<u16>(m_metric_count - 1);
    u16 const advance = u16_at(m_hmtx.offset + static_cast<size_t>(index) * 4);
    if (!m_is_varied)
        return advance;
    if (auto const* known = m_varied_advances.get(glyph))
        return *known;
    // The third and fourth of the extra points are its width: how far apart they've moved is how much it's changed by.
    Vector<u16> ends;
    Vector<float> xs;
    Vector<float> ys;
    u32 const points = point_count_of(glyph, ends, xs, ys);
    Vector<float> dx;
    Vector<float> dy;
    u16 result = advance;
    if (points >= 4 && variation_deltas(glyph, points, ends.span(), xs.span(), ys.span(), dx, dy)) {
        float const changed = static_cast<float>(advance) + dx[points - 3] - dx[points - 4];
        result = static_cast<u16>(max(0.0f, changed + 0.5f));
    }
    (void)m_varied_advances.try_set(glyph, result);
    return result;
}

// ---- Variations (fvar, avar, gvar).

void Typeface::read_variations()
{
    if (!m_fvar.exists() || u16_at(m_fvar.offset) != 1)
        return;
    size_t const axes = m_fvar.offset + u16_at(m_fvar.offset + 4);
    u16 const count = u16_at(m_fvar.offset + 8);
    u16 const size = u16_at(m_fvar.offset + 10);
    if (size < 20 || count > 64)
        return;
    auto const fixed = [this](size_t at) { return static_cast<float>(static_cast<i32>(u32_at(at))) / 65536.0f; };
    for (u16 i = 0; i < count; ++i) {
        size_t const axis = axes + static_cast<size_t>(i) * size;
        m_variation_axes.append({ u32_at(axis), fixed(axis + 4), fixed(axis + 8), fixed(axis + 12) });
        m_normalized_coordinates.append(0);
    }
}

ErrorOr<NonnullRefPtr<Typeface>> Typeface::with_variations(Span<VariationSetting const> settings) const
{
    auto varied = TRY(parse_mapped(m_data));
    for (size_t a = 0; a < varied->m_variation_axes.size(); ++a) {
        auto const& axis = varied->m_variation_axes[a];
        float value = axis.default_value;
        for (auto const& setting : settings) {
            if (setting.tag == axis.tag)
                value = setting.value;
        }
        value = max(axis.minimum, min(axis.maximum, value));
        // To between -1 and 1, with the default at 0.
        float normalized = 0;
        if (value < axis.default_value && axis.default_value > axis.minimum)
            normalized = (value - axis.default_value) / (axis.default_value - axis.minimum);
        else if (value > axis.default_value && axis.maximum > axis.default_value)
            normalized = (value - axis.default_value) / (axis.maximum - axis.default_value);
        // And the font's own idea of how that goes.
        if (varied->m_avar.exists()) {
            size_t at = varied->m_avar.offset + 8;
            for (size_t skip = 0; skip < a; ++skip)
                at += 2 + static_cast<size_t>(varied->u16_at(at)) * 4;
            u16 const pairs = varied->u16_at(at);
            auto const from_at = [&](u16 i) { return static_cast<float>(varied->i16_at(at + 2 + static_cast<size_t>(i) * 4)) / 16384.0f; };
            auto const to_at = [&](u16 i) { return static_cast<float>(varied->i16_at(at + 4 + static_cast<size_t>(i) * 4)) / 16384.0f; };
            for (u16 i = 0; i + 1 < pairs; ++i) {
                float const from = from_at(i);
                float const next = from_at(static_cast<u16>(i + 1));
                if (normalized >= from && normalized <= next && next > from) {
                    normalized = to_at(i) + (normalized - from) / (next - from) * (to_at(static_cast<u16>(i + 1)) - to_at(i));
                    break;
                }
            }
        }
        varied->m_normalized_coordinates[a] = normalized;
        if (normalized != 0)
            varied->m_is_varied = varied->m_gvar.exists();
    }
    return varied;
}

float Typeface::variation_scalar(size_t peak, size_t intermediate) const
{
    float scalar = 1;
    size_t const axes = m_normalized_coordinates.size();
    for (size_t a = 0; a < axes; ++a) {
        float const peak_value = static_cast<float>(i16_at(peak + a * 2)) / 16384.0f;
        if (peak_value == 0)
            continue;
        float const value = m_normalized_coordinates[a];
        if (value == 0)
            return 0;
        if (intermediate != 0) {
            float const low = static_cast<float>(i16_at(intermediate + a * 2)) / 16384.0f;
            float const high = static_cast<float>(i16_at(intermediate + axes * 2 + a * 2)) / 16384.0f;
            if (value < low || value > high)
                return 0;
            if (value == peak_value)
                continue;
            scalar *= value < peak_value ? (value - low) / (peak_value - low) : (high - value) / (high - peak_value);
        } else {
            if ((value < 0) != (peak_value < 0))
                return 0;
            if (value < 0 ? value < peak_value : value > peak_value)
                continue;
            scalar *= value / peak_value;
        }
    }
    return scalar;
}

u32 Typeface::point_count_of(GlyphIndex glyph, Vector<u16>& contour_ends, Vector<float>& xs, Vector<float>& ys) const
{
    size_t start, end;
    if (m_long_loca) {
        start = u32_at(m_loca.offset + static_cast<size_t>(glyph) * 4);
        end = u32_at(m_loca.offset + (static_cast<size_t>(glyph) + 1) * 4);
    } else {
        start = static_cast<size_t>(u16_at(m_loca.offset + static_cast<size_t>(glyph) * 2)) * 2;
        end = static_cast<size_t>(u16_at(m_loca.offset + (static_cast<size_t>(glyph) + 1) * 2)) * 2;
    }
    u32 count = 0;
    if (end > start && end <= m_glyf.size && end - start >= 10) {
        size_t const offset = m_glyf.offset + start;
        size_t const limit = m_glyf.offset + end;
        i16 const contours = i16_at(offset);
        if (contours >= 0) {
            if (contours > 0)
                count = static_cast<u32>(u16_at(offset + 10 + (static_cast<size_t>(contours) - 1) * 2)) + 1;
            for (i16 c = 0; c < contours; ++c)
                contour_ends.append(u16_at(offset + 10 + static_cast<size_t>(c) * 2));
        } else {
            size_t at = offset + 10;
            for (int part = 0; part < 64 && at + 4 <= limit; ++part) {
                u16 const flags = u16_at(at);
                ++count;
                at += 4 + ((flags & 1) ? 4 : 2);
                if (flags & 0x0008)
                    at += 2;
                else if (flags & 0x0040)
                    at += 4;
                else if (flags & 0x0080)
                    at += 8;
                if (!(flags & 0x0020))
                    break;
            }
        }
    }
    // Only for what's looked at to fill in points that aren't said: and those are only asked for when there are contours, by what actually has them.
    for (u32 i = 0; i < count + 4; ++i) {
        xs.append(0);
        ys.append(0);
    }
    return count + 4;
}

bool Typeface::variation_deltas(GlyphIndex glyph, u32 point_count, Span<u16 const> contour_ends, Span<float const> xs, Span<float const> ys, Vector<float>& dx, Vector<float>& dy) const
{
    if (!m_is_varied || point_count > 65535)
        return false;
    size_t const axes = m_normalized_coordinates.size();
    if (u16_at(m_gvar.offset + 4) != axes)
        return false;
    u16 const shared_tuple_count = u16_at(m_gvar.offset + 6);
    size_t const shared_tuples = m_gvar.offset + u32_at(m_gvar.offset + 8);
    bool const long_offsets = u16_at(m_gvar.offset + 14) & 1;
    size_t const data_start = m_gvar.offset + u32_at(m_gvar.offset + 16);
    size_t const offsets = m_gvar.offset + 20;
    if (glyph >= u16_at(m_gvar.offset + 12))
        return false;
    size_t const first = long_offsets ? u32_at(offsets + static_cast<size_t>(glyph) * 4) : static_cast<size_t>(u16_at(offsets + static_cast<size_t>(glyph) * 2)) * 2;
    size_t const next = long_offsets ? u32_at(offsets + (static_cast<size_t>(glyph) + 1) * 4) : static_cast<size_t>(u16_at(offsets + (static_cast<size_t>(glyph) + 1) * 2)) * 2;
    if (next <= first)
        return false;
    size_t const data = data_start + first;
    size_t const data_end = min(data_start + next, m_data.size());

    u16 const tuple_info = u16_at(data);
    u16 const tuple_count = tuple_info & 0x0fff;
    size_t serialized = data + u16_at(data + 2);
    size_t header = data + 4;

    // Points are said as how far each is from the last.
    auto const read_points = [&](size_t& at, Vector<u16>& points) -> bool {
        u32 count = byte_at(at++);
        if (count & 0x80)
            count = ((count & 0x7f) << 8) | byte_at(at++);
        // None said means all of them.
        if (count == 0) {
            points.clear();
            return true;
        }
        u32 value = 0;
        while (points.size() < count) {
            if (at >= data_end)
                return false;
            u8 const control = byte_at(at++);
            u32 const run = (control & 0x7f) + 1u;
            for (u32 i = 0; i < run && points.size() < count; ++i) {
                if (control & 0x80) {
                    value += u16_at(at);
                    at += 2;
                } else {
                    value += byte_at(at++);
                }
                points.append(static_cast<u16>(value));
            }
        }
        return true;
    };
    auto const read_deltas = [&](size_t& at, u32 count, Vector<float>& out) -> bool {
        while (out.size() < count) {
            if (at >= data_end)
                return false;
            u8 const control = byte_at(at++);
            u32 const run = (control & 0x3f) + 1u;
            for (u32 i = 0; i < run && out.size() < count; ++i) {
                if (control & 0x80) {
                    out.append(0);
                } else if (control & 0x40) {
                    out.append(static_cast<float>(i16_at(at)));
                    at += 2;
                } else {
                    out.append(static_cast<float>(static_cast<i8>(byte_at(at++))));
                }
            }
        }
        return true;
    };

    Vector<u16> shared_points;
    bool has_shared_points = false;
    if (tuple_info & 0x8000) {
        if (!read_points(serialized, shared_points))
            return false;
        has_shared_points = true;
    }

    MUST(dx.try_resize(point_count));
    MUST(dy.try_resize(point_count));
    for (u32 i = 0; i < point_count; ++i) {
        dx[i] = 0;
        dy[i] = 0;
    }
    bool anything = false;
    for (u16 t = 0; t < tuple_count; ++t) {
        u16 const data_size = u16_at(header);
        u16 const index = u16_at(header + 2);
        header += 4;
        size_t peak;
        if (index & 0x8000) {
            peak = header;
            header += axes * 2;
        } else {
            if ((index & 0x0fff) >= shared_tuple_count)
                return anything;
            peak = shared_tuples + static_cast<size_t>(index & 0x0fff) * axes * 2;
        }
        size_t intermediate = 0;
        if (index & 0x4000) {
            intermediate = header;
            header += axes * 4;
        }
        size_t const next_serialized = serialized + data_size;
        float const scalar = variation_scalar(peak, intermediate);
        if (scalar != 0) {
            size_t at = serialized;
            Vector<u16> own_points;
            if (index & 0x2000) {
                if (!read_points(at, own_points))
                    return anything;
            }
            auto const& points = (index & 0x2000) ? own_points : shared_points;
            bool const all_points = points.is_empty() && ((index & 0x2000) || has_shared_points || true);
            u32 const said = all_points ? point_count : static_cast<u32>(points.size());
            Vector<float> x_deltas;
            Vector<float> y_deltas;
            if (!read_deltas(at, said, x_deltas) || !read_deltas(at, said, y_deltas))
                return anything;
            if (all_points) {
                for (u32 i = 0; i < point_count; ++i) {
                    dx[i] += x_deltas[i] * scalar;
                    dy[i] += y_deltas[i] * scalar;
                }
            } else {
                // Those it says, and those between them by following along (IUP): each contour by itself, in what's left over by the end.
                Vector<float> tuple_dx;
                Vector<float> tuple_dy;
                Vector<bool> touched;
                MUST(tuple_dx.try_resize(point_count));
                MUST(tuple_dy.try_resize(point_count));
                MUST(touched.try_resize(point_count));
                for (u32 i = 0; i < point_count; ++i) {
                    tuple_dx[i] = 0;
                    tuple_dy[i] = 0;
                    touched[i] = false;
                }
                for (u32 i = 0; i < said; ++i) {
                    if (points[i] < point_count) {
                        tuple_dx[points[i]] = x_deltas[i];
                        tuple_dy[points[i]] = y_deltas[i];
                        touched[points[i]] = true;
                    }
                }
                u32 contour_start = 0;
                for (u16 end_of_contour : contour_ends) {
                    if (end_of_contour >= point_count || end_of_contour < contour_start)
                        break;
                    u32 const size = end_of_contour - contour_start + 1u;
                    // The first that's touched: if there isn't one, nothing moves.
                    u32 first_touched = size;
                    for (u32 i = 0; i < size; ++i) {
                        if (touched[contour_start + i]) {
                            first_touched = i;
                            break;
                        }
                    }
                    if (first_touched < size) {
                        u32 previous = first_touched;
                        for (u32 step = 1; step <= size; ++step) {
                            u32 const here = (first_touched + step) % size;
                            if (!touched[contour_start + here])
                                continue;
                            // Everything from just after `previous` to just before `here`.
                            auto const interpolate = [&](Span<float const> position, Vector<float>& delta) {
                                float const p1 = position[contour_start + previous];
                                float const p2 = position[contour_start + here];
                                float const d1 = delta[contour_start + previous];
                                float const d2 = delta[contour_start + here];
                                for (u32 k = (previous + 1) % size; k != here; k = (k + 1) % size) {
                                    float const p = position[contour_start + k];
                                    float value;
                                    if (p1 == p2) {
                                        value = d1 == d2 ? d1 : 0;
                                    } else if (p <= min(p1, p2)) {
                                        value = p1 < p2 ? d1 : d2;
                                    } else if (p >= max(p1, p2)) {
                                        value = p1 > p2 ? d1 : d2;
                                    } else {
                                        value = d1 + (p - p1) * (d2 - d1) / (p2 - p1);
                                    }
                                    delta[contour_start + k] = value;
                                }
                            };
                            interpolate(xs, tuple_dx);
                            interpolate(ys, tuple_dy);
                            previous = here;
                        }
                    }
                    contour_start = end_of_contour + 1u;
                }
                for (u32 i = 0; i < point_count; ++i) {
                    dx[i] += tuple_dx[i] * scalar;
                    dy[i] += tuple_dy[i] * scalar;
                }
            }
            anything = true;
        }
        serialized = next_serialized;
    }
    return anything;
}

// ---- Glyphs that are pictures: COLR and CBDT.

Vector<Typeface::ColorLayer> Typeface::color_layers(GlyphIndex glyph) const
{
    Vector<ColorLayer> layers;
    if (!m_colr.exists() || !m_cpal.exists())
        return layers;
    u16 const base_count = u16_at(m_colr.offset + 2);
    size_t const bases = m_colr.offset + u32_at(m_colr.offset + 4);
    size_t const all_layers = m_colr.offset + u32_at(m_colr.offset + 8);
    u32 low = 0, high = base_count;
    while (low < high) {
        u32 const middle = (low + high) / 2;
        size_t const record = bases + static_cast<size_t>(middle) * 6;
        u16 const value = u16_at(record);
        if (value == glyph) {
            u16 const first = u16_at(record + 2);
            u16 const count = u16_at(record + 4);
            size_t const palette_start = m_cpal.offset + u32_at(m_cpal.offset + 8) + static_cast<size_t>(u16_at(m_cpal.offset + 12)) * 4;
            u16 const palette_size = u16_at(m_cpal.offset + 2);
            for (u16 l = 0; l < count && l < 256; ++l) {
                size_t const layer = all_layers + (static_cast<size_t>(first) + l) * 4;
                ColorLayer one;
                one.glyph = u16_at(layer);
                u16 const palette_index = u16_at(layer + 2);
                if (palette_index == 0xffff || palette_index >= palette_size) {
                    one.is_text_color = true;
                } else {
                    // BGRA, as it's kept.
                    size_t const color = palette_start + static_cast<size_t>(palette_index) * 4;
                    one.argb = (static_cast<u32>(byte_at(color + 3)) << 24) | (static_cast<u32>(byte_at(color + 2)) << 16) | (static_cast<u32>(byte_at(color + 1)) << 8) | byte_at(color);
                }
                layers.append(one);
            }
            return layers;
        }
        if (value < glyph)
            low = middle + 1;
        else
            high = middle;
    }
    return layers;
}

Optional<Typeface::ColorBitmap> Typeface::color_bitmap(GlyphIndex glyph, u16 wanted) const
{
    if (!m_cbdt.exists() || !m_cblc.exists())
        return {};
    u32 const strikes = u32_at(m_cblc.offset + 4);
    if (strikes > 256)
        return {};
    // The smallest that's at least as big, or the biggest there is.
    size_t best = 0;
    u8 best_size = 0;
    for (u32 s = 0; s < strikes; ++s) {
        size_t const strike = m_cblc.offset + 8 + static_cast<size_t>(s) * 48;
        u16 const start = u16_at(strike + 40);
        u16 const end = u16_at(strike + 42);
        if (glyph < start || glyph > end)
            continue;
        u8 const size = byte_at(strike + 44);
        bool const better = best == 0 || (best_size < wanted ? size > best_size : (size >= wanted && size < best_size));
        if (better) {
            best = strike;
            best_size = size;
        }
    }
    if (best == 0)
        return {};
    size_t const sub_tables = m_cblc.offset + u32_at(best);
    u32 const sub_table_count = u32_at(best + 8);
    if (sub_table_count > 65536)
        return {};
    for (u32 t = 0; t < sub_table_count; ++t) {
        size_t const record = sub_tables + static_cast<size_t>(t) * 8;
        u16 const first = u16_at(record);
        u16 const last = u16_at(record + 2);
        if (glyph < first || glyph > last)
            continue;
        size_t const sub = sub_tables + u32_at(record + 4);
        u16 const index_format = u16_at(sub);
        u16 const image_format = u16_at(sub + 2);
        size_t const image_base = u32_at(sub + 4);
        size_t offset = 0;
        size_t next = 0;
        if (index_format == 1) {
            offset = u32_at(sub + 8 + static_cast<size_t>(glyph - first) * 4);
            next = u32_at(sub + 8 + (static_cast<size_t>(glyph - first) + 1) * 4);
        } else if (index_format == 3) {
            offset = u16_at(sub + 8 + static_cast<size_t>(glyph - first) * 2);
            next = u16_at(sub + 8 + (static_cast<size_t>(glyph - first) + 1) * 2);
        } else {
            return {};
        }
        if (next <= offset)
            return {};
        size_t const data = m_cbdt.offset + image_base + offset;
        ColorBitmap bitmap;
        bitmap.pixels_per_em = best_size;
        if (image_format == 17) {
            // Small metrics, then its length, then the PNG.
            bitmap.left = static_cast<i8>(byte_at(data + 2));
            bitmap.top = static_cast<i8>(byte_at(data + 3));
            bitmap.advance = byte_at(data + 4);
            u32 const length = u32_at(data + 5);
            if (data + 9 + length > m_data.size())
                return {};
            bitmap.png = { m_data.data() + data + 9, length };
        } else if (image_format == 18) {
            bitmap.left = static_cast<i8>(byte_at(data + 2));
            bitmap.top = static_cast<i8>(byte_at(data + 3));
            bitmap.advance = byte_at(data + 4);
            u32 const length = u32_at(data + 8);
            if (data + 12 + length > m_data.size())
                return {};
            bitmap.png = { m_data.data() + data + 12, length };
        } else {
            return {};
        }
        return bitmap;
    }
    return {};
}

// --- Outlines ---

Gfx::Path Typeface::outline_of(GlyphIndex glyph) const
{
    Gfx::Path path;
    if (glyph >= m_glyph_count)
        return path;
    if (m_cff.exists())
        append_cff_outline(glyph, path);
    else if (m_glyf.exists())
        append_truetype_outline(glyph, path, {}, 0);
    return path;
}

void Typeface::append_truetype_outline(GlyphIndex glyph, Gfx::Path& path, Gfx::AffineTransform const& transform, int depth) const
{
    if (glyph >= m_glyph_count || depth > 6)
        return;
    size_t start, end;
    if (m_long_loca) {
        start = u32_at(m_loca.offset + static_cast<size_t>(glyph) * 4);
        end = u32_at(m_loca.offset + (static_cast<size_t>(glyph) + 1) * 4);
    } else {
        start = static_cast<size_t>(u16_at(m_loca.offset + static_cast<size_t>(glyph) * 2)) * 2;
        end = static_cast<size_t>(u16_at(m_loca.offset + (static_cast<size_t>(glyph) + 1) * 2)) * 2;
    }
    if (end <= start || end > m_glyf.size || end - start < 10)
        return; // No outline: a space.
    size_t offset = m_glyf.offset + start;
    size_t limit = m_glyf.offset + end;
    i16 contours = i16_at(offset);
    if (contours >= 0) {
        append_simple_outline(glyph, offset + 10, limit, contours, path, transform);
        return;
    }
    // Where its parts go can change with the design, as anything can: each part is a "point" here, with what's left over being the glyph's metrics.
    Vector<float> part_dx;
    Vector<float> part_dy;
    if (m_is_varied) {
        Vector<u16> no_contours;
        Vector<float> xs;
        Vector<float> ys;
        u32 const parts = point_count_of(glyph, no_contours, xs, ys);
        if (parts > 0)
            (void)variation_deltas(glyph, parts, no_contours.span(), xs.span(), ys.span(), part_dx, part_dy);
    }
    // Composite: other glyphs, each moved and perhaps scaled.
    constexpr u16 arguments_are_words = 0x0001;
    constexpr u16 arguments_are_offsets = 0x0002;
    constexpr u16 has_scale = 0x0008;
    constexpr u16 more_components = 0x0020;
    constexpr u16 has_xy_scale = 0x0040;
    constexpr u16 has_matrix = 0x0080;
    size_t at = offset + 10;
    for (int component = 0; component < 64; ++component) {
        if (at + 4 > limit)
            return;
        u16 flags = u16_at(at);
        u16 child = u16_at(at + 2);
        at += 4;
        float dx = 0, dy = 0;
        if (flags & arguments_are_words) {
            dx = i16_at(at);
            dy = i16_at(at + 2);
            at += 4;
        } else {
            dx = static_cast<i8>(byte_at(at));
            dy = static_cast<i8>(byte_at(at + 1));
            at += 2;
        }
        // (Point matching, without offsets, is rare enough to place at the origin.)
        if (!(flags & arguments_are_offsets))
            dx = dy = 0;
        if (static_cast<size_t>(component) < part_dx.size()) {
            dx += part_dx[static_cast<size_t>(component)];
            dy += part_dy[static_cast<size_t>(component)];
        }
        float a = 1, b = 0, c = 0, d = 1;
        auto fixed = [&](size_t where) { return static_cast<float>(i16_at(where)) / 16384.0f; };
        if (flags & has_scale) {
            a = d = fixed(at);
            at += 2;
        } else if (flags & has_xy_scale) {
            a = fixed(at);
            d = fixed(at + 2);
            at += 4;
        } else if (flags & has_matrix) {
            a = fixed(at);
            b = fixed(at + 2);
            c = fixed(at + 4);
            d = fixed(at + 6);
            at += 8;
        }
        append_truetype_outline(child, path, transform.multiplied(Gfx::AffineTransform(a, b, c, d, dx, dy)), depth + 1);
        if (!(flags & more_components))
            return;
    }
}

void Typeface::append_simple_outline(GlyphIndex glyph, size_t offset, size_t limit, i16 contours, Gfx::Path& path, Gfx::AffineTransform const& transform) const
{
    if (contours == 0 || offset + static_cast<size_t>(contours) * 2 + 2 > limit)
        return;
    u32 point_count = static_cast<u32>(u16_at(offset + (static_cast<size_t>(contours) - 1) * 2)) + 1;
    size_t at = offset + static_cast<size_t>(contours) * 2;
    u16 instructions = u16_at(at);
    at += 2u + instructions;
    if (at > limit)
        return;

    constexpr u8 on_curve = 0x01, x_short = 0x02, y_short = 0x04, repeat = 0x08, x_same = 0x10, y_same = 0x20;
    struct OutlinePoint {
        float x;
        float y;
        bool on;
    };
    Vector<u8> flags;
    if (flags.try_resize(point_count).is_error())
        return;
    for (u32 i = 0; i < point_count;) {
        if (at >= limit)
            return;
        u8 flag = byte_at(at++);
        flags[i++] = flag;
        if (flag & repeat) {
            if (at >= limit)
                return;
            u32 times = byte_at(at++);
            while (times-- && i < point_count)
                flags[i++] = flag;
        }
    }
    Vector<OutlinePoint> points;
    if (points.try_resize(point_count).is_error())
        return;
    int value = 0;
    for (u32 i = 0; i < point_count; ++i) {
        u8 flag = flags[i];
        if (flag & x_short) {
            int delta = byte_at(at++);
            value += (flag & x_same) ? delta : -delta;
        } else if (!(flag & x_same)) {
            value += i16_at(at);
            at += 2;
        }
        points[i].x = static_cast<float>(value);
        points[i].on = flag & on_curve;
    }
    value = 0;
    for (u32 i = 0; i < point_count; ++i) {
        u8 flag = flags[i];
        if (flag & y_short) {
            int delta = byte_at(at++);
            value += (flag & y_same) ? delta : -delta;
        } else if (!(flag & y_same)) {
            value += i16_at(at);
            at += 2;
        }
        points[i].y = static_cast<float>(value);
    }
    if (at > limit)
        return;

    if (m_is_varied) {
        Vector<u16> ends;
        Vector<float> xs;
        Vector<float> ys;
        for (i16 contour = 0; contour < contours; ++contour)
            ends.append(u16_at(offset + static_cast<size_t>(contour) * 2));
        for (auto const& point : points) {
            xs.append(point.x);
            ys.append(point.y);
        }
        // (The four at the end, for the metrics, aren't in the outline.)
        xs.append(0);
        xs.append(0);
        xs.append(0);
        xs.append(0);
        ys.append(0);
        ys.append(0);
        ys.append(0);
        ys.append(0);
        Vector<float> dx;
        Vector<float> dy;
        if (variation_deltas(glyph, point_count + 4, ends.span(), xs.span(), ys.span(), dx, dy)) {
            for (u32 i = 0; i < point_count; ++i) {
                points[i].x += dx[i];
                points[i].y += dy[i];
            }
        }
    }

    u32 first = 0;
    for (i16 contour = 0; contour < contours; ++contour) {
        u32 last = u16_at(offset + static_cast<size_t>(contour) * 2);
        if (last < first || last >= point_count)
            return;
        u32 count = last - first + 1;
        auto point_at = [&](u32 index) -> OutlinePoint const& { return points[first + index % count]; };
        auto mapped = [&](float x, float y) { return transform.map(Gfx::FloatPoint { x, y }); };
        // TrueType's quadratics: between two off-curve points lies an implied
        // on-curve one. Start at an on-curve point, or at such a midpoint.
        u32 start = 0;
        Gfx::FloatPoint start_point;
        if (point_at(0).on) {
            start_point = { point_at(0).x, point_at(0).y };
            start = 1;
        } else if (point_at(count - 1).on) {
            start_point = { point_at(count - 1).x, point_at(count - 1).y };
            start = 0;
            --count;
            // (The last point is the start; no need to come back to it twice.)
        } else {
            start_point = { (point_at(0).x + point_at(count - 1).x) / 2, (point_at(0).y + point_at(count - 1).y) / 2 };
        }
        path.move_to(mapped(start_point.x, start_point.y));
        Optional<Gfx::FloatPoint> control;
        for (u32 i = start; i < count; ++i) {
            auto const& point = point_at(i);
            Gfx::FloatPoint here { point.x, point.y };
            if (point.on) {
                if (control.has_value()) {
                    path.quad_to(mapped(control->x, control->y), mapped(here.x, here.y));
                    control = {};
                } else {
                    path.line_to(mapped(here.x, here.y));
                }
            } else {
                if (control.has_value()) {
                    Gfx::FloatPoint middle { (control->x + here.x) / 2, (control->y + here.y) / 2 };
                    path.quad_to(mapped(control->x, control->y), mapped(middle.x, middle.y));
                }
                control = here;
            }
        }
        if (control.has_value())
            path.quad_to(mapped(control->x, control->y), mapped(start_point.x, start_point.y));
        path.close();
        first = last + 1;
    }
}

// --- CFF (Type 2 charstrings) ---

Typeface::CFFIndex Typeface::read_cff_index(size_t at) const
{
    CFFIndex index;
    index.count = u16_at(at);
    if (index.count == 0) {
        index.end = at + 2;
        return index;
    }
    index.offset_size = byte_at(at + 2);
    if (index.offset_size < 1 || index.offset_size > 4) {
        index.count = 0;
        index.end = at + 2;
        return index;
    }
    index.offsets = at + 3;
    index.data = index.offsets + (static_cast<size_t>(index.count) + 1) * index.offset_size - 1;
    size_t start = 0, end = 0;
    if (!cff_object(index, index.count - 1, start, end)) {
        index.count = 0;
        index.end = at + 2;
        return index;
    }
    index.end = end;
    return index;
}

bool Typeface::cff_object(CFFIndex const& index, u32 which, size_t& start, size_t& end) const
{
    if (which >= index.count)
        return false;
    auto offset_at = [&](u32 entry) {
        size_t value = 0;
        for (u8 i = 0; i < index.offset_size; ++i)
            value = (value << 8) | byte_at(index.offsets + static_cast<size_t>(entry) * index.offset_size + i);
        return value;
    };
    start = index.data + offset_at(which);
    end = index.data + offset_at(which + 1);
    return end >= start && end <= m_data.size();
}

void Typeface::read_cff()
{
    size_t base = m_cff.offset;
    size_t header_size = byte_at(base + 2);
    auto names = read_cff_index(base + header_size);
    auto top_dicts = read_cff_index(names.end);
    auto strings = read_cff_index(top_dicts.end);
    m_cff_global_subroutines = read_cff_index(strings.end);
    size_t start = 0, end = 0;
    if (!cff_object(top_dicts, 0, start, end))
        return;
    // A DICT: operands, then an operator.
    size_t charstrings_offset = 0;
    size_t private_size = 0, private_offset = 0;
    // A CID-keyed font, as those for Chinese and Japanese are: several dictionaries, each with its own subroutines, and a list of which glyph goes by which.
    size_t font_dictionaries_offset = 0;
    size_t dictionary_select_offset = 0;
    auto walk_dict = [&](size_t from, size_t to, auto on_operator) {
        double operands[48];
        int count = 0;
        size_t at = from;
        while (at < to) {
            u8 byte = byte_at(at++);
            if (byte <= 21) {
                u16 op = byte;
                if (byte == 12)
                    op = static_cast<u16>(1200 + byte_at(at++));
                on_operator(op, operands, count);
                count = 0;
            } else {
                double value = 0;
                if (byte == 28) {
                    value = i16_at(at);
                    at += 2;
                } else if (byte == 29) {
                    value = static_cast<i32>(u32_at(at));
                    at += 4;
                } else if (byte == 30) {
                    // A real number: only skipped over here.
                    while (at < to) {
                        u8 nibbles = byte_at(at++);
                        if ((nibbles & 15) == 15 || (nibbles >> 4) == 15)
                            break;
                    }
                } else if (byte >= 32 && byte <= 246) {
                    value = byte - 139;
                } else if (byte >= 247 && byte <= 250) {
                    value = (byte - 247) * 256 + byte_at(at++) + 108;
                } else if (byte >= 251 && byte <= 254) {
                    value = -(byte - 251) * 256 - byte_at(at++) - 108;
                }
                if (count < 48)
                    operands[count++] = value;
            }
        }
    };
    walk_dict(start, end, [&](u16 op, double const* operands, int count) {
        if (op == 17 && count >= 1)
            charstrings_offset = static_cast<size_t>(operands[0]);
        if (op == 18 && count >= 2) {
            private_size = static_cast<size_t>(operands[0]);
            private_offset = static_cast<size_t>(operands[1]);
        }
        if (op == 1236 && count >= 1)
            font_dictionaries_offset = static_cast<size_t>(operands[0]);
        if (op == 1237 && count >= 1)
            dictionary_select_offset = static_cast<size_t>(operands[0]);
    });
    if (charstrings_offset == 0)
        return;
    m_cff_charstrings = read_cff_index(base + charstrings_offset);
    if (font_dictionaries_offset != 0 && dictionary_select_offset != 0) {
        auto const dictionaries = read_cff_index(base + font_dictionaries_offset);
        // (No more than this has ever been needed: an index is only a byte, and most have a few dozen.)
        for (u32 d = 0; d < dictionaries.count && d < 256; ++d) {
            CFFIndex local;
            size_t dictionary_start = 0, dictionary_end = 0;
            if (cff_object(dictionaries, d, dictionary_start, dictionary_end)) {
                size_t size = 0, offset = 0;
                walk_dict(dictionary_start, dictionary_end, [&](u16 op, double const* operands, int count) {
                    if (op == 18 && count >= 2) {
                        size = static_cast<size_t>(operands[0]);
                        offset = static_cast<size_t>(operands[1]);
                    }
                });
                if (size && offset && base + offset + size <= m_data.size()) {
                    size_t subroutines = 0;
                    walk_dict(base + offset, base + offset + size, [&](u16 op, double const* operands, int count) {
                        if (op == 19 && count >= 1)
                            subroutines = static_cast<size_t>(operands[0]);
                    });
                    if (subroutines)
                        local = read_cff_index(base + offset + subroutines);
                }
            }
            m_cff_font_dictionary_locals.append(local);
        }
        m_cff_dictionary_select = base + dictionary_select_offset;
        return;
    }
    if (private_size && private_offset && base + private_offset + private_size <= m_data.size()) {
        size_t subroutines = 0;
        walk_dict(base + private_offset, base + private_offset + private_size, [&](u16 op, double const* operands, int count) {
            if (op == 19 && count >= 1)
                subroutines = static_cast<size_t>(operands[0]);
        });
        if (subroutines)
            m_cff_local_subroutines = read_cff_index(base + private_offset + subroutines);
    }
}

struct CharstringState {
    Gfx::Path* path { nullptr };
    float stack[64] {};
    int count { 0 };
    float x { 0 };
    float y { 0 };
    int stems { 0 };
    bool width_seen { false };
    bool open { false };
    bool done { false };
    int budget { 200000 };
};

void Typeface::run_charstring(size_t start, size_t end, CharstringState& state, CFFIndex const& local, int depth) const
{
    if (depth > 10)
        return;
    auto push = [&](float value) {
        if (state.count < 64)
            state.stack[state.count++] = value;
    };
    auto move_to = [&](float dx, float dy) {
        if (state.open)
            state.path->close();
        state.x += dx;
        state.y += dy;
        state.path->move_to({ state.x, state.y });
        state.open = true;
    };
    auto line_to = [&](float dx, float dy) {
        state.x += dx;
        state.y += dy;
        state.path->line_to({ state.x, state.y });
    };
    auto curve_to = [&](float dx1, float dy1, float dx2, float dy2, float dx3, float dy3) {
        Gfx::FloatPoint first { state.x + dx1, state.y + dy1 };
        Gfx::FloatPoint second { first.x + dx2, first.y + dy2 };
        state.x = second.x + dx3;
        state.y = second.y + dy3;
        state.path->cubic_to(first, second, { state.x, state.y });
    };
    // The first operator that clears the stack may have the width in front.
    auto take_width = [&](bool has_extra) {
        if (!state.width_seen) {
            state.width_seen = true;
            if (has_extra) {
                for (int i = 1; i < state.count; ++i)
                    state.stack[i - 1] = state.stack[i];
                --state.count;
            }
        }
    };
    auto subroutine = [&](CFFIndex const& index) {
        if (state.count == 0)
            return;
        int bias = index.count < 1240 ? 107 : (index.count < 33900 ? 1131 : 32768);
        int which = static_cast<int>(state.stack[--state.count]) + bias;
        size_t from = 0, to = 0;
        if (which >= 0 && cff_object(index, static_cast<u32>(which), from, to))
            run_charstring(from, to, state, local, depth + 1);
    };

    size_t at = start;
    while (at < end && !state.done) {
        if (--state.budget <= 0) {
            state.done = true;
            return;
        }
        u8 byte = byte_at(at++);
        if (byte >= 32 || byte == 28) {
            if (byte == 28) {
                push(i16_at(at));
                at += 2;
            } else if (byte <= 246) {
                push(byte - 139);
            } else if (byte <= 250) {
                push(static_cast<float>((byte - 247) * 256 + byte_at(at++) + 108));
            } else if (byte <= 254) {
                push(static_cast<float>(-(byte - 251) * 256 - byte_at(at++) - 108));
            } else {
                push(static_cast<float>(static_cast<i32>(u32_at(at))) / 65536.0f);
                at += 4;
            }
            continue;
        }
        float const* s = state.stack;
        int n = state.count;
        bool clear = true;
        switch (byte) {
        case 1:  // hstem
        case 3:  // vstem
        case 18: // hstemhm
        case 23: // vstemhm
            take_width(n % 2 == 1);
            state.stems += state.count / 2;
            break;
        case 19: // hintmask
        case 20: // cntrmask
            take_width(n % 2 == 1);
            state.stems += state.count / 2;
            at += static_cast<size_t>(state.stems + 7) / 8;
            break;
        case 21: // rmoveto
            take_width(n > 2);
            if (state.count >= 2)
                move_to(s[0], s[1]);
            break;
        case 22: // hmoveto
            take_width(n > 1);
            if (state.count >= 1)
                move_to(s[0], 0);
            break;
        case 4: // vmoveto
            take_width(n > 1);
            if (state.count >= 1)
                move_to(0, s[0]);
            break;
        case 5: // rlineto
            for (int i = 0; i + 2 <= n; i += 2)
                line_to(s[i], s[i + 1]);
            break;
        case 6:   // hlineto
        case 7: { // vlineto
            bool horizontal = byte == 6;
            for (int i = 0; i < n; ++i) {
                line_to(horizontal ? s[i] : 0, horizontal ? 0 : s[i]);
                horizontal = !horizontal;
            }
            break;
        }
        case 8: // rrcurveto
            for (int i = 0; i + 6 <= n; i += 6)
                curve_to(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            break;
        case 24: { // rcurveline
            int i = 0;
            for (; i + 8 <= n; i += 6)
                curve_to(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            if (i + 2 <= n)
                line_to(s[i], s[i + 1]);
            break;
        }
        case 25: { // rlinecurve
            int i = 0;
            for (; i + 8 <= n; i += 2)
                line_to(s[i], s[i + 1]);
            if (i + 6 <= n)
                curve_to(s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            break;
        }
        case 26: { // vvcurveto
            int i = 0;
            float dx1 = 0;
            if (n % 4 == 1)
                dx1 = s[i++];
            for (; i + 4 <= n; i += 4) {
                curve_to(dx1, s[i], s[i + 1], s[i + 2], 0, s[i + 3]);
                dx1 = 0;
            }
            break;
        }
        case 27: { // hhcurveto
            int i = 0;
            float dy1 = 0;
            if (n % 4 == 1)
                dy1 = s[i++];
            for (; i + 4 <= n; i += 4) {
                curve_to(s[i], dy1, s[i + 1], s[i + 2], s[i + 3], 0);
                dy1 = 0;
            }
            break;
        }
        case 30:   // vhcurveto
        case 31: { // hvcurveto
            bool horizontal = byte == 31;
            int i = 0;
            while (i + 4 <= n) {
                float last = (n - i == 5) ? s[i + 4] : 0;
                if (horizontal)
                    curve_to(s[i], 0, s[i + 1], s[i + 2], last, s[i + 3]);
                else
                    curve_to(0, s[i], s[i + 1], s[i + 2], s[i + 3], last);
                horizontal = !horizontal;
                i += 4;
            }
            break;
        }
        case 10: // callsubr
            subroutine(local);
            clear = false;
            break;
        case 29: // callgsubr
            subroutine(m_cff_global_subroutines);
            clear = false;
            break;
        case 11: // return
            return;
        case 14: // endchar
            take_width(n == 1 || n == 5);
            if (state.open)
                state.path->close();
            state.open = false;
            state.done = true;
            return;
        case 12: {
            u8 second = byte_at(at++);
            switch (second) {
            case 34: // hflex
                if (n >= 7) {
                    curve_to(s[0], 0, s[1], s[2], s[3], 0);
                    curve_to(s[4], 0, s[5], -s[2], s[6], 0);
                }
                break;
            case 35: // flex
                if (n >= 12) {
                    curve_to(s[0], s[1], s[2], s[3], s[4], s[5]);
                    curve_to(s[6], s[7], s[8], s[9], s[10], s[11]);
                }
                break;
            case 36: // hflex1
                if (n >= 9) {
                    curve_to(s[0], s[1], s[2], s[3], s[4], 0);
                    curve_to(s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
                }
                break;
            case 37: // flex1
                if (n >= 11) {
                    float dx = s[0] + s[2] + s[4] + s[6] + s[8];
                    float dy = s[1] + s[3] + s[5] + s[7] + s[9];
                    bool across = Base::Math::absolute(dx) > Base::Math::absolute(dy);
                    curve_to(s[0], s[1], s[2], s[3], s[4], s[5]);
                    curve_to(s[6], s[7], s[8], s[9], across ? s[10] : -dx, across ? -dy : s[10]);
                }
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
        if (clear)
            state.count = 0;
    }
}

void Typeface::append_cff_outline(GlyphIndex glyph, Gfx::Path& path) const
{
    size_t start = 0, end = 0;
    if (!cff_object(m_cff_charstrings, glyph, start, end))
        return;
    CharstringState state;
    state.path = &path;
    run_charstring(start, end, state, cff_local_subroutines_for(glyph), 0);
    if (state.open)
        path.close();
}

Typeface::CFFIndex const& Typeface::cff_local_subroutines_for(GlyphIndex glyph) const
{
    if (m_cff_dictionary_select == 0)
        return m_cff_local_subroutines;
    size_t which = 0;
    u8 const format = byte_at(m_cff_dictionary_select);
    if (format == 0) {
        which = byte_at(m_cff_dictionary_select + 1 + glyph);
    } else if (format == 3) {
        u16 const ranges = u16_at(m_cff_dictionary_select + 1);
        // Each is where it starts, and which: the last that starts at or before this one.
        u32 low = 0, high = ranges;
        while (low < high) {
            u32 const middle = (low + high) / 2;
            if (u16_at(m_cff_dictionary_select + 3 + static_cast<size_t>(middle) * 3) <= glyph)
                low = middle + 1;
            else
                high = middle;
        }
        if (low > 0)
            which = byte_at(m_cff_dictionary_select + 3 + static_cast<size_t>(low - 1) * 3 + 2);
    }
    return which < m_cff_font_dictionary_locals.size() ? m_cff_font_dictionary_locals[which] : m_cff_local_subroutines;
}

// --- Layout tables ---

// The features of the script we use (Latin, else the default one) in its default
// language: for each, its tag and the lookups it names.
void Typeface::read_layout_features(Table const& table, Vector<u32>& tags, Vector<Vector<u16>>& lookups) const
{
    if (!table.exists() || table.size < 10)
        return;
    size_t scripts = table.offset + u16_at(table.offset + 4);
    size_t features = table.offset + u16_at(table.offset + 6);
    u16 script_count = u16_at(scripts);
    size_t chosen = 0;
    for (u16 i = 0; i < script_count; ++i) {
        size_t record = scripts + 2 + static_cast<size_t>(i) * 6;
        u32 name = u32_at(record);
        size_t offset = scripts + u16_at(record + 4);
        if (name == feature_tag("latn")) {
            chosen = offset;
            break;
        }
        if (name == feature_tag("DFLT"))
            chosen = offset;
    }
    if (!chosen)
        return;
    u16 default_language = u16_at(chosen);
    if (default_language == 0)
        return;
    size_t language = chosen + default_language;
    u16 feature_count = u16_at(language + 4);
    u16 total_features = u16_at(features);
    for (u16 i = 0; i < feature_count; ++i) {
        u16 feature_index = u16_at(language + 6 + static_cast<size_t>(i) * 2);
        if (feature_index >= total_features)
            continue;
        size_t record = features + 2 + static_cast<size_t>(feature_index) * 6;
        u32 name = u32_at(record);
        size_t feature = features + u16_at(record + 4);
        u16 lookup_count = u16_at(feature + 2);
        Vector<u16> indexes;
        for (u16 l = 0; l < lookup_count; ++l)
            indexes.append(u16_at(feature + 4 + static_cast<size_t>(l) * 2));
        tags.append(name);
        lookups.append(move(indexes));
    }
}

int Typeface::coverage_index(size_t coverage, GlyphIndex glyph) const
{
    u16 format = u16_at(coverage);
    u16 count = u16_at(coverage + 2);
    if (format == 1) {
        int low = 0, high = count;
        while (low < high) {
            int middle = (low + high) / 2;
            u16 value = u16_at(coverage + 4 + static_cast<size_t>(middle) * 2);
            if (glyph < value)
                high = middle;
            else if (glyph > value)
                low = middle + 1;
            else
                return middle;
        }
    } else if (format == 2) {
        int low = 0, high = count;
        while (low < high) {
            int middle = (low + high) / 2;
            size_t range = coverage + 4 + static_cast<size_t>(middle) * 6;
            if (glyph < u16_at(range))
                high = middle;
            else if (glyph > u16_at(range + 2))
                low = middle + 1;
            else
                return u16_at(range + 4) + (glyph - u16_at(range));
        }
    }
    return -1;
}

u16 Typeface::class_of(size_t definition, GlyphIndex glyph) const
{
    u16 format = u16_at(definition);
    if (format == 1) {
        u16 first = u16_at(definition + 2);
        u16 count = u16_at(definition + 4);
        if (glyph >= first && glyph - first < count)
            return u16_at(definition + 6 + static_cast<size_t>(glyph - first) * 2);
    } else if (format == 2) {
        u16 count = u16_at(definition + 2);
        int low = 0, high = count;
        while (low < high) {
            int middle = (low + high) / 2;
            size_t range = definition + 4 + static_cast<size_t>(middle) * 6;
            if (glyph < u16_at(range))
                high = middle;
            else if (glyph > u16_at(range + 2))
                low = middle + 1;
            else
                return u16_at(range + 4);
        }
    }
    return 0;
}

i16 Typeface::pair_adjustment(size_t subtable, GlyphIndex left, GlyphIndex right, bool& found) const
{
    u16 format = u16_at(subtable);
    int covered = coverage_index(subtable + u16_at(subtable + 2), left);
    if (covered < 0)
        return 0;
    u16 first_format = u16_at(subtable + 4);
    u16 second_format = u16_at(subtable + 6);
    auto value_size = [](u16 value_format) { return static_cast<size_t>(__builtin_popcount(value_format)) * 2; };
    // Of the first glyph's value record, the advance, if it has one.
    auto advance_in = [&](size_t record) -> i16 {
        if (!(first_format & 0x0004))
            return 0;
        size_t skip = (first_format & 1) ? 2 : 0;
        skip += (first_format & 2) ? 2 : 0;
        return i16_at(record + skip);
    };
    if (format == 1) {
        size_t set = subtable + u16_at(subtable + 10 + static_cast<size_t>(covered) * 2);
        u16 count = u16_at(set);
        size_t record_size = 2 + value_size(first_format) + value_size(second_format);
        int low = 0, high = count;
        while (low < high) {
            int middle = (low + high) / 2;
            size_t record = set + 2 + static_cast<size_t>(middle) * record_size;
            u16 second = u16_at(record);
            if (right < second) {
                high = middle;
            } else if (right > second) {
                low = middle + 1;
            } else {
                found = true;
                return advance_in(record + 2);
            }
        }
    } else if (format == 2) {
        size_t first_classes = subtable + u16_at(subtable + 8);
        size_t second_classes = subtable + u16_at(subtable + 10);
        u16 first_count = u16_at(subtable + 12);
        u16 second_count = u16_at(subtable + 14);
        u16 first_class = class_of(first_classes, left);
        u16 second_class = class_of(second_classes, right);
        if (first_class >= first_count || second_class >= second_count)
            return 0;
        size_t record_size = value_size(first_format) + value_size(second_format);
        size_t record = subtable + 16 + (static_cast<size_t>(first_class) * second_count + second_class) * record_size;
        // Class 0 of the second glyph means "not in a class": a subtable matches
        // whatever it covers, so later ones are not looked at.
        found = true;
        return advance_in(record);
    }
    return 0;
}

i16 Typeface::gpos_kerning(GlyphIndex left, GlyphIndex right) const
{
    size_t lookup_list = m_gpos.offset + u16_at(m_gpos.offset + 8);
    u16 lookup_count = u16_at(lookup_list);
    i16 total = 0;
    for (u16 index : m_kerning_lookups) {
        if (index >= lookup_count)
            continue;
        size_t lookup = lookup_list + u16_at(lookup_list + 2 + static_cast<size_t>(index) * 2);
        u16 type = u16_at(lookup);
        u16 subtables = u16_at(lookup + 4);
        bool found = false;
        for (u16 i = 0; i < subtables && !found; ++i) {
            size_t subtable = lookup + u16_at(lookup + 6 + static_cast<size_t>(i) * 2);
            u16 kind = type;
            if (kind == 9) { // Extension: the real one is elsewhere.
                kind = u16_at(subtable + 2);
                subtable += u32_at(subtable + 4);
            }
            if (kind == 2)
                total = static_cast<i16>(total + pair_adjustment(subtable, left, right, found));
        }
    }
    return total;
}

i16 Typeface::kern_table_kerning(GlyphIndex left, GlyphIndex right) const
{
    if (!m_kern.exists())
        return 0;
    u16 tables = u16_at(m_kern.offset + 2);
    size_t at = m_kern.offset + 4;
    for (u16 i = 0; i < tables; ++i) {
        u16 length = u16_at(at + 2);
        u16 coverage = u16_at(at + 4);
        if ((coverage >> 8) == 0 && (coverage & 1)) {
            u16 pairs = u16_at(at + 6);
            u32 wanted = (static_cast<u32>(left) << 16) | right;
            int low = 0, high = pairs;
            while (low < high) {
                int middle = (low + high) / 2;
                size_t record = at + 14 + static_cast<size_t>(middle) * 6;
                u32 pair = u32_at(record);
                if (wanted < pair)
                    high = middle;
                else if (wanted > pair)
                    low = middle + 1;
                else
                    return i16_at(record + 4);
            }
        }
        at += length;
        if (length == 0)
            break;
    }
    return 0;
}

i16 Typeface::kerning(GlyphIndex left, GlyphIndex right) const
{
    u32 key = (static_cast<u32>(left) << 16) | right;
    if (auto const* known = m_kerning_cache.get(key))
        return *known;
    i16 value = m_kerning_lookups.is_empty() ? kern_table_kerning(left, right) : gpos_kerning(left, right);
    // The cache may fail to grow: it only costs looking it up again.
    (void)m_kerning_cache.try_set(key, value);
    return value;
}

// --- Substitutions ---

void Typeface::substitute(Vector<GlyphIndex>& glyphs, Span<u32 const> wanted_features) const
{
    if (!m_gsub.exists() || glyphs.is_empty())
        return;
    Vector<u16> lookups;
    for (size_t i = 0; i < m_gsub_features.size(); ++i) {
        bool wanted = false;
        for (size_t f = 0; f < wanted_features.size(); ++f)
            wanted = wanted || wanted_features[f] == m_gsub_features[i];
        if (!wanted)
            continue;
        for (u16 lookup : m_gsub_feature_lookups[i]) {
            if (!lookups.contains_slow(lookup))
                lookups.append(lookup);
        }
    }
    quick_sort(lookups.span());
    for (u16 lookup : lookups)
        apply_lookup(lookup, glyphs);
}

void Typeface::apply_lookup(u16 lookup_index, Vector<GlyphIndex>& glyphs) const
{
    for (size_t position = 0; position < glyphs.size(); ++position)
        (void)apply_single_at(lookup_index, glyphs, position);
}

bool Typeface::apply_single_at(u16 lookup_index, Vector<GlyphIndex>& glyphs, size_t position) const
{
    // A damaged font's lookups may call one another forever.
    if (m_substitution_depth >= 8)
        return false;
    ++m_substitution_depth;
    bool applied = apply_single_at_inner(lookup_index, glyphs, position);
    --m_substitution_depth;
    return applied;
}

bool Typeface::apply_single_at_inner(u16 lookup_index, Vector<GlyphIndex>& glyphs, size_t position) const
{
    size_t lookup_list = m_gsub.offset + u16_at(m_gsub.offset + 8);
    if (lookup_index >= u16_at(lookup_list))
        return false;
    size_t lookup = lookup_list + u16_at(lookup_list + 2 + static_cast<size_t>(lookup_index) * 2);
    u16 type = u16_at(lookup);
    u16 subtables = u16_at(lookup + 4);
    for (u16 i = 0; i < subtables; ++i) {
        size_t subtable = lookup + u16_at(lookup + 6 + static_cast<size_t>(i) * 2);
        u16 kind = type;
        if (kind == 7) { // Extension.
            kind = u16_at(subtable + 2);
            subtable += u32_at(subtable + 4);
        }
        size_t at = position;
        if (apply_subtable(kind, subtable, glyphs, at))
            return true;
    }
    return false;
}

bool Typeface::apply_subtable(u16 type, size_t subtable, Vector<GlyphIndex>& glyphs, size_t& position) const
{
    u16 format = u16_at(subtable);
    GlyphIndex glyph = glyphs[position];
    switch (type) {
    case 1: { // Single.
        int covered = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (covered < 0)
            return false;
        if (format == 1) {
            glyphs[position] = static_cast<GlyphIndex>(glyph + i16_at(subtable + 4));
        } else if (format == 2) {
            if (covered >= u16_at(subtable + 4))
                return false;
            glyphs[position] = u16_at(subtable + 6 + static_cast<size_t>(covered) * 2);
        } else {
            return false;
        }
        return true;
    }
    case 2: { // Multiple: one glyph becomes several.
        int covered = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (covered < 0 || format != 1 || covered >= u16_at(subtable + 4))
            return false;
        size_t sequence = subtable + u16_at(subtable + 6 + static_cast<size_t>(covered) * 2);
        u16 count = u16_at(sequence);
        if (count == 0 || glyphs.size() + count > 65536)
            return false;
        glyphs[position] = u16_at(sequence + 2);
        for (u16 i = 1; i < count; ++i)
            glyphs.insert(position + i, u16_at(sequence + 2 + static_cast<size_t>(i) * 2));
        position += static_cast<size_t>(count) - 1;
        return true;
    }
    case 4: { // Ligature: several glyphs become one.
        int covered = coverage_index(subtable + u16_at(subtable + 2), glyph);
        if (covered < 0 || format != 1 || covered >= u16_at(subtable + 4))
            return false;
        size_t set = subtable + u16_at(subtable + 6 + static_cast<size_t>(covered) * 2);
        u16 ligatures = u16_at(set);
        for (u16 i = 0; i < ligatures; ++i) {
            size_t ligature = set + u16_at(set + 2 + static_cast<size_t>(i) * 2);
            u16 components = u16_at(ligature + 2);
            if (components == 0 || position + components > glyphs.size())
                continue;
            bool matches = true;
            for (u16 c = 1; c < components && matches; ++c)
                matches = glyphs[position + c] == u16_at(ligature + 4 + static_cast<size_t>(c - 1) * 2);
            if (!matches)
                continue;
            glyphs[position] = u16_at(ligature);
            for (u16 c = 1; c < components; ++c)
                glyphs.remove(position + 1);
            return true;
        }
        return false;
    }
    case 6: { // Chained context: what is before, at and after decides.
        // Runs the substitutions a matching rule names.
        auto apply_rule = [&](size_t record_start, u16 count) {
            for (u16 i = 0; i < count; ++i) {
                size_t record = record_start + static_cast<size_t>(i) * 4;
                u16 sequence_index = u16_at(record);
                u16 nested = u16_at(record + 2);
                if (position + sequence_index < glyphs.size())
                    (void)apply_single_at(nested, glyphs, position + sequence_index);
            }
        };
        if (format == 1 || format == 2) {
            // Rules listing the glyphs (1) or their classes (2) around this one.
            int covered = coverage_index(subtable + u16_at(subtable + 2), glyph);
            if (covered < 0)
                return false;
            size_t backtrack_classes = format == 2 ? subtable + u16_at(subtable + 4) : 0;
            size_t input_classes = format == 2 ? subtable + u16_at(subtable + 6) : 0;
            size_t lookahead_classes = format == 2 ? subtable + u16_at(subtable + 8) : 0;
            size_t sets_record = format == 2 ? subtable + 10 : subtable + 4;
            u16 set_count = u16_at(sets_record);
            u32 which = format == 2 ? class_of(input_classes, glyph) : static_cast<u32>(covered);
            if (which >= set_count)
                return false;
            u16 set_offset = u16_at(sets_record + 2 + static_cast<size_t>(which) * 2);
            if (set_offset == 0)
                return false;
            size_t set = subtable + set_offset;
            u16 rules = u16_at(set);
            auto matches = [&](GlyphIndex candidate, u16 wanted, size_t classes) {
                return format == 2 ? class_of(classes, candidate) == wanted : candidate == wanted;
            };
            for (u16 r = 0; r < rules; ++r) {
                size_t rule = set + u16_at(set + 2 + static_cast<size_t>(r) * 2);
                u16 backtrack = u16_at(rule);
                size_t backtrack_list = rule + 2;
                size_t input_record = backtrack_list + static_cast<size_t>(backtrack) * 2;
                u16 input = u16_at(input_record); // Counting this glyph.
                size_t input_list = input_record + 2;
                size_t lookahead_record = input_list + static_cast<size_t>(input ? input - 1 : 0) * 2;
                u16 lookahead = u16_at(lookahead_record);
                size_t lookahead_list = lookahead_record + 2;
                size_t apply_record = lookahead_list + static_cast<size_t>(lookahead) * 2;
                if (input == 0 || position < backtrack || position + input + lookahead > glyphs.size())
                    continue;
                bool ok = true;
                for (u16 i = 0; i < backtrack && ok; ++i)
                    ok = matches(glyphs[position - 1 - i], u16_at(backtrack_list + static_cast<size_t>(i) * 2), backtrack_classes);
                for (u16 i = 1; i < input && ok; ++i)
                    ok = matches(glyphs[position + i], u16_at(input_list + static_cast<size_t>(i - 1) * 2), input_classes);
                for (u16 i = 0; i < lookahead && ok; ++i)
                    ok = matches(glyphs[position + input + i], u16_at(lookahead_list + static_cast<size_t>(i) * 2), lookahead_classes);
                if (!ok)
                    continue;
                apply_rule(apply_record + 2, u16_at(apply_record));
                return true;
            }
            return false;
        }
        if (format != 3)
            return false;
        u16 backtrack = u16_at(subtable + 2);
        size_t backtrack_coverages = subtable + 4;
        size_t input_record = backtrack_coverages + static_cast<size_t>(backtrack) * 2;
        u16 input = u16_at(input_record);
        size_t input_coverages = input_record + 2;
        size_t lookahead_record = input_coverages + static_cast<size_t>(input) * 2;
        u16 lookahead = u16_at(lookahead_record);
        size_t lookahead_coverages = lookahead_record + 2;
        size_t apply_record = lookahead_coverages + static_cast<size_t>(lookahead) * 2;
        u16 applications = u16_at(apply_record);
        if (input == 0 || position < backtrack || position + input + lookahead > glyphs.size())
            return false;
        for (u16 i = 0; i < backtrack; ++i) {
            if (coverage_index(subtable + u16_at(backtrack_coverages + static_cast<size_t>(i) * 2), glyphs[position - 1 - i]) < 0)
                return false;
        }
        for (u16 i = 0; i < input; ++i) {
            if (coverage_index(subtable + u16_at(input_coverages + static_cast<size_t>(i) * 2), glyphs[position + i]) < 0)
                return false;
        }
        for (u16 i = 0; i < lookahead; ++i) {
            if (coverage_index(subtable + u16_at(lookahead_coverages + static_cast<size_t>(i) * 2), glyphs[position + input + i]) < 0)
                return false;
        }
        apply_rule(apply_record + 2, applications);
        return true;
    }
    default:
        return false;
    }
}

}
