/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibText/Itemizer.h>
#include <LibText/Paragraph.h>
#include <LibText/Shaper.h>

namespace Text {

using Unicode::CodePoint;

namespace {

struct Item {
    size_t start { 0 };
    size_t end { 0 };
    u32 style { 0 };
    Unicode::Script script { Unicode::Script::Latin };
    u8 level { 0 };
    Typeface const* typeface { nullptr };
};

struct Decoded {
    Vector<CodePoint> points;
    // Which byte each starts at, and one more for the end.
    Vector<u32> byte_offsets;
};

ErrorOr<Decoded> decode(StringView text)
{
    Decoded out;
    for (size_t at = 0; at < text.length();) {
        auto const decoded = Unicode::decode_utf8(text.substring_view(at));
        TRY(out.byte_offsets.try_append(static_cast<u32>(at)));
        TRY(out.points.try_append(decoded.code_point));
        at += decoded.length;
    }
    TRY(out.byte_offsets.try_append(static_cast<u32>(text.length())));
    return out;
}

// Which style a character's in.
u32 style_of_byte(Span<StyleRange const> ranges, size_t byte)
{
    for (auto const& range : ranges) {
        if (byte >= range.start && byte < range.end)
            return range.style;
    }
    return ranges.is_empty() ? 0 : ranges[ranges.size() - 1].style;
}

}

ErrorOr<ParagraphLayout> ParagraphLayout::create(StringView text, Span<StyleRange const> ranges, Span<ParagraphStyle const> styles, float width, ParagraphAlignment alignment, Unicode::ParagraphDirection direction)
{
    ParagraphLayout layout;
    layout.m_text_length = text.length();
    if (styles.is_empty())
        return Error::from_string_literal("a paragraph needs a style");

    auto const decoded = TRY(decode(text));
    auto const& points = decoded.points;
    auto const& offsets = decoded.byte_offsets;

    auto const paragraph = TRY(Unicode::resolve_bidi(points.span(), direction));
    layout.m_is_right_to_left = paragraph.paragraph_level & 1;

    // For each character: which script, style and font. These don't change with where a line ends.
    Vector<u32> char_styles;
    Vector<Unicode::Script> scripts;
    Vector<Typeface const*> faces;
    TRY(char_styles.try_resize(points.size()));
    TRY(scripts.try_resize(points.size()));
    TRY(faces.try_resize(points.size()));
    {
        auto current = Unicode::Script::Common;
        for (size_t i = 0; i < points.size(); ++i) {
            char_styles[i] = style_of_byte(ranges, offsets[i]);
            if (char_styles[i] >= styles.size())
                char_styles[i] = 0;
            auto const script = Unicode::script_of(points[i]);
            if (script != Unicode::Script::Common && script != Unicode::Script::Inherited && script != Unicode::Script::Unknown)
                current = script;
            scripts[i] = current;
        }
        auto after = Unicode::Script::Latin;
        for (size_t i = points.size(); i > 0; --i) {
            if (scripts[i - 1] == Unicode::Script::Common)
                scripts[i - 1] = after;
            else
                after = scripts[i - 1];
        }
        auto const clusters = TRY(Unicode::grapheme_boundaries(points.span()));
        for (size_t b = 0; b + 1 < clusters.size(); ++b) {
            size_t const from = clusters[b];
            size_t const to = clusters[b + 1];
            auto const& primary = *styles[char_styles[from]].typeface;
            Typeface const* chosen = nullptr;
            if (to - from >= 2 && points[from + 1] == 0xfe0f)
                chosen = FontCascade::the().emoji_typeface_for(points[from]);
            for (size_t i = from; i < to && !chosen; ++i) {
                if (!Unicode::is_default_ignorable(points[i]))
                    chosen = FontCascade::the().typeface_for(primary, points[i], scripts[i]);
            }
            for (size_t i = from; i < to; ++i)
                faces[i] = chosen ? chosen : &primary;
        }
    }

    // A line's worth, from `from` up to `to`, laid out: in the order it's drawn, from the left.
    auto const make_line = [&](size_t from, size_t to) -> ErrorOr<Line> {
        Line line;
        line.start = offsets[from];
        line.end = offsets[to];
        auto const levels = TRY(Unicode::reordering_levels(paragraph, from, to));

        // Runs of everything alike, in the text's order.
        Vector<Item> logical;
        for (size_t i = from; i < to;) {
            u8 const level = levels[i - from] == Unicode::bidi_level_removed ? static_cast<u8>(paragraph.paragraph_level) : levels[i - from];
            size_t j = i + 1;
            while (j < to) {
                u8 const next_level = levels[j - from] == Unicode::bidi_level_removed ? level : levels[j - from];
                if (next_level != level || scripts[j] != scripts[i] || faces[j] != faces[i] || char_styles[j] != char_styles[i])
                    break;
                ++j;
            }
            TRY(logical.try_append({ i, j, char_styles[i], scripts[i], level, faces[i] }));
            i = j;
        }
        Vector<u8> run_levels;
        for (auto const& item : logical)
            TRY(run_levels.try_append(item.level));
        auto const order = TRY(Unicode::visual_order(run_levels.span()));

        // A line's as tall as the tallest thing on it, even if there's nothing on it.
        auto const& first_style = styles[from < points.size() ? char_styles[from] : (points.is_empty() ? 0 : char_styles[points.size() - 1])];
        auto const measure = [&](Typeface const& face, float size) {
            float const scale = size / static_cast<float>(face.units_per_em());
            line.ascent = max(line.ascent, static_cast<float>(face.ascent()) * scale);
            line.descent = max(line.descent, static_cast<float>(-face.descent()) * scale);
            line.line_gap = max(line.line_gap, static_cast<float>(face.line_gap()) * scale);
        };
        measure(*first_style.typeface, first_style.points);

        float pen = 0;
        for (size_t place : order) {
            auto const& item = logical[place];
            auto const& style = styles[item.style];
            auto const& face = *item.typeface;
            float const scale = style.points / static_cast<float>(face.units_per_em());
            measure(face, style.points);
            ShapeRequest request;
            request.text = points.span().slice(item.start, item.end - item.start);
            request.script = item.script;
            request.is_right_to_left = item.level & 1;
            auto const shaped = TRY(Text::shape(face, request));
            Run run;
            run.style = item.style;
            run.x = pen;
            run.is_right_to_left = item.level & 1;
            float run_pen = 0;
            TRY(run.glyphs.try_ensure_capacity(shaped.size()));
            for (auto const& glyph : shaped) {
                PositionedGlyph placed;
                placed.glyph = glyph.glyph;
                placed.x = run_pen + static_cast<float>(glyph.x_offset) * scale;
                placed.y = -static_cast<float>(glyph.y_offset) * scale;
                placed.typeface = &face;
                placed.advance = static_cast<float>(glyph.x_advance) * scale;
                placed.is_right_to_left = run.is_right_to_left;
                size_t const point = item.start + glyph.cluster;
                placed.cluster = point < offsets.size() ? offsets[point] : static_cast<u32>(text.length());
                run.glyphs.append(placed);
                run_pen += placed.advance;
            }
            run.width = run_pen;
            pen += run_pen;
            TRY(line.runs.try_append(move(run)));
        }
        line.width = pen;
        return line;
    };

    // Where it can end. Each line takes as much as fits, going by what's been shaped for it: which is what it comes to, and not an estimate, as letters take on other shapes at the end of a line.
    auto const opportunities = TRY(Unicode::line_break_opportunities(points.span()));
    size_t line_start = 0;
    size_t next_opportunity = 0;
    auto const trailing_space = [&](size_t from, size_t to) {
        // What's at the end of a line, and doesn't count towards whether it fits.
        size_t end = to;
        while (end > from && (Unicode::is_white_space(points[end - 1])))
            --end;
        return end;
    };
    while (line_start < points.size() || (points.is_empty() && layout.m_lines.is_empty())) {
        if (points.is_empty()) {
            TRY(layout.m_lines.try_append(TRY(make_line(0, 0))));
            break;
        }
        size_t best = 0;
        bool found = false;
        // What's ahead, up to where the line has to end anyway.
        while (next_opportunity < opportunities.size() && opportunities[next_opportunity].position <= line_start)
            ++next_opportunity;
        size_t last_candidate = next_opportunity;
        while (last_candidate + 1 < opportunities.size() && !opportunities[last_candidate].mandatory)
            ++last_candidate;
        if (next_opportunity < opportunities.size()) {
            auto const fits = [&](size_t candidate) -> ErrorOr<bool> {
                if (width <= 0)
                    return true;
                auto const measured = TRY(make_line(line_start, trailing_space(line_start, opportunities[candidate].position)));
                return measured.width <= width;
            };
            if (TRY(fits(last_candidate))) {
                best = opportunities[last_candidate].position;
                found = true;
                next_opportunity = last_candidate + 1;
            } else if (TRY(fits(next_opportunity))) {
                size_t low = next_opportunity;
                size_t high = last_candidate;
                while (low + 1 < high) {
                    size_t const middle = (low + high) / 2;
                    if (TRY(fits(middle)))
                        low = middle;
                    else
                        high = middle;
                }
                best = opportunities[low].position;
                found = true;
                next_opportunity = low + 1;
            }
        }
        if (!found) {
            // Not even one word fits. As many whole characters as do, then.
            auto const clusters = TRY(Unicode::grapheme_boundaries(points.span().slice(line_start, points.size() - line_start)));
            size_t take = clusters.size() > 1 ? clusters[1] : 1;
            for (size_t c = 2; c < clusters.size(); ++c) {
                auto const measured = TRY(make_line(line_start, line_start + clusters[c]));
                if (measured.width > width)
                    break;
                take = clusters[c];
            }
            best = line_start + take;
        }
        // (Spaces at the end stay in the line's text, for the caret, but take up no room in it.)
        auto line = TRY(make_line(line_start, trailing_space(line_start, best)));
        line.end = offsets[best];
        TRY(layout.m_lines.try_append(move(line)));
        line_start = best;
    }
    // Text that ends with a line break has one more line after it, with nothing on it.
    if (!points.is_empty() && (points[points.size() - 1] == '\n' || points[points.size() - 1] == 0x2029 || points[points.size() - 1] == 0x2028)) {
        auto line = TRY(make_line(points.size(), points.size()));
        line.start = line.end = text.length();
        TRY(layout.m_lines.try_append(move(line)));
    }

    // Top to bottom, and across.
    float y = 0;
    for (auto& line : layout.m_lines) {
        line.baseline = y + line.ascent;
        y += line.height();
        layout.m_widest = max(layout.m_widest, line.width);
        float const room = (width > 0 ? width : 0);
        switch (alignment) {
        case ParagraphAlignment::Natural:
            line.x = layout.m_is_right_to_left && room > 0 ? room - line.width : 0;
            break;
        case ParagraphAlignment::Left:
            line.x = 0;
            break;
        case ParagraphAlignment::Center:
            line.x = room > 0 ? (room - line.width) / 2 : 0;
            break;
        case ParagraphAlignment::Right:
            line.x = room > 0 ? room - line.width : 0;
            break;
        }
    }
    layout.m_height = y;
    return layout;
}

size_t ParagraphLayout::line_of(size_t offset) const
{
    for (size_t i = 0; i < m_lines.size(); ++i) {
        if (offset < m_lines[i].end)
            return i;
    }
    return m_lines.is_empty() ? 0 : m_lines.size() - 1;
}

float ParagraphLayout::caret_x(size_t offset) const
{
    if (m_lines.is_empty())
        return 0;
    auto const& line = m_lines[line_of(offset)];
    // The start of whatever's at this place, or past it: on the side that's its start.
    Optional<float> found;
    u32 nearest = 0xffffffff;
    float end_of_last = line.x + (m_is_right_to_left ? 0 : line.width);
    u32 last_cluster = 0;
    bool any = false;
    for (auto const& run : line.runs) {
        float pen = line.x + run.x;
        for (auto const& glyph : run.glyphs) {
            float const left = pen;
            float const right = pen + glyph.advance;
            pen = right;
            if (glyph.cluster >= offset && glyph.cluster < nearest) {
                nearest = glyph.cluster;
                found = glyph.is_right_to_left ? right : left;
            } else if (glyph.cluster == nearest && found.has_value()) {
                found = glyph.is_right_to_left ? max(found.value(), right) : min(found.value(), left);
            }
            if (!any || glyph.cluster >= last_cluster) {
                last_cluster = glyph.cluster;
                end_of_last = glyph.is_right_to_left ? left : right;
                any = true;
            }
        }
    }
    return found.has_value() ? found.value() : end_of_last;
}

size_t ParagraphLayout::offset_at(float x, float y) const
{
    if (m_lines.is_empty())
        return 0;
    size_t which = m_lines.size() - 1;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        if (y < m_lines[i].top() + m_lines[i].height()) {
            which = i;
            break;
        }
    }
    auto const& line = m_lines[which];
    // Past either end: that end of the line, in the text's own order.
    if (line.runs.is_empty())
        return line.start;
    auto const last_offset_on_line = [&] {
        // Before any line break at the end of it, or the space that the next line follows.
        return which + 1 < m_lines.size() ? line.end - (line.end > line.start ? 1 : 0) : line.end;
    };
    if (x < line.x)
        return m_is_right_to_left ? last_offset_on_line() : line.start;
    for (auto const& run : line.runs) {
        float pen = line.x + run.x;
        for (auto const& glyph : run.glyphs) {
            float const right = pen + glyph.advance;
            if (x < right) {
                bool const first_half = x < pen + glyph.advance / 2;
                if (first_half != glyph.is_right_to_left)
                    return glyph.cluster;
                // The next place after this cluster, among everything on the line.
                u32 next = static_cast<u32>(line.end);
                for (auto const& other_run : line.runs) {
                    for (auto const& other : other_run.glyphs) {
                        if (other.cluster > glyph.cluster && other.cluster < next)
                            next = other.cluster;
                    }
                }
                return next;
            }
            pen = right;
        }
    }
    return m_is_right_to_left ? line.start : last_offset_on_line();
}

Vector<ParagraphLayout::Span1D> ParagraphLayout::selection_spans(size_t start, size_t end) const
{
    Vector<Span1D> spans;
    for (size_t index = 0; index < m_lines.size(); ++index) {
        auto const& line = m_lines[index];
        if (end <= line.start || start >= line.end)
            continue;
        bool has_current = false;
        float left = 0;
        float right = 0;
        for (auto const& run : line.runs) {
            float pen = line.x + run.x;
            for (auto const& glyph : run.glyphs) {
                float const glyph_right = pen + glyph.advance;
                bool const selected = glyph.cluster >= start && glyph.cluster < end;
                if (selected) {
                    if (!has_current) {
                        left = pen;
                        has_current = true;
                    }
                    right = glyph_right;
                } else if (has_current) {
                    spans.append({ left, right, index });
                    has_current = false;
                }
                pen = glyph_right;
            }
        }
        if (has_current)
            spans.append({ left, right, index });
    }
    return spans;
}

}
