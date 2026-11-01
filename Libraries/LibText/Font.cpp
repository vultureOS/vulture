/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibGfx/Rasterizer.h>
#include <LibText/Font.h>

namespace Text {

namespace Math = Base::Math;

ErrorOr<GlyphAtlas::Slot> GlyphAtlas::allocate(Gfx::IntSize size, u8*& writable, int& pitch)
{
    if (size.width > page_size || size.height > page_size)
        return Error::from_string_literal("a glyph too large for the atlas");
    // A pixel between images, so nothing bleeds if they are ever filtered.
    constexpr int gutter = 1;
    auto place = [&](Page& page) -> Optional<Slot> {
        if (page.shelf_x + size.width + gutter > page_size) {
            page.shelf_y += page.shelf_height + gutter;
            page.shelf_x = 0;
            page.shelf_height = 0;
        }
        if (page.shelf_y + size.height > page_size)
            return {};
        Gfx::IntRect area { page.shelf_x, page.shelf_y, size.width, size.height };
        page.shelf_x += size.width + gutter;
        page.shelf_height = max(page.shelf_height, size.height);
        writable = page.mask->scanline(area.y) + area.x;
        pitch = page.mask->width();
        return Slot { page.mask.ptr(), area };
    };
    if (!m_pages.is_empty()) {
        if (auto slot = place(m_pages.last()); slot.has_value())
            return slot.release_value();
    }
    auto mask = TRY(Gfx::AlphaMask::create({ page_size, page_size }));
    TRY(m_pages.try_append(Page { move(mask), 0, 0, 0 }));
    auto slot = place(m_pages.last());
    if (!slot.has_value())
        return Error::from_string_literal("a glyph too large for the atlas");
    return slot.release_value();
}

Font::Font(NonnullRefPtr<Typeface> typeface, float points, float scale, GlyphAtlas& atlas)
    : m_typeface(move(typeface))
    , m_points(points)
    , m_scale(scale)
    , m_pixels_per_unit(points * scale / static_cast<float>(m_typeface->units_per_em()))
    , m_atlas(atlas)
{
}

float Font::ascent() const { return static_cast<float>(m_typeface->ascent()) * m_pixels_per_unit; }
float Font::descent() const { return static_cast<float>(-m_typeface->descent()) * m_pixels_per_unit; }
float Font::line_height() const { return static_cast<float>(m_typeface->ascent() - m_typeface->descent() + m_typeface->line_gap()) * m_pixels_per_unit; }
float Font::cap_height() const { return static_cast<float>(m_typeface->cap_height()) * m_pixels_per_unit; }
float Font::x_height() const { return static_cast<float>(m_typeface->x_height()) * m_pixels_per_unit; }

namespace {

// The next code point of UTF-8, or U+FFFD for what is not valid.
u32 next_code_point(StringView text, size_t& at)
{
    auto lead = static_cast<u8>(text[at++]);
    if (lead < 0x80)
        return lead;
    int continuation = 0;
    u32 value = 0;
    u32 smallest = 0;
    if ((lead & 0xe0) == 0xc0) {
        continuation = 1;
        value = lead & 0x1f;
        smallest = 0x80;
    } else if ((lead & 0xf0) == 0xe0) {
        continuation = 2;
        value = lead & 0x0f;
        smallest = 0x800;
    } else if ((lead & 0xf8) == 0xf0) {
        continuation = 3;
        value = lead & 0x07;
        smallest = 0x10000;
    } else {
        return 0xfffd;
    }
    for (int i = 0; i < continuation; ++i) {
        if (at >= text.length() || (static_cast<u8>(text[at]) & 0xc0) != 0x80)
            return 0xfffd;
        value = (value << 6) | (static_cast<u8>(text[at++]) & 0x3f);
    }
    if (value < smallest || value > 0x10ffff || (value >= 0xd800 && value < 0xe000))
        return 0xfffd;
    return value;
}

}

ShapedRun Font::shape(StringView text) const
{
    ShapedRun run;
    Vector<GlyphIndex> glyphs;
    for (size_t at = 0; at < text.length();)
        glyphs.append(m_typeface->glyph_for_code_point(next_code_point(text, at)));
    // On by default in text, as CoreText has them. (No contextual alternates:
    // those turn "->" into an arrow, which is a choice for the reader.)
    static constexpr u32 features[] = { feature_tag("ccmp"), feature_tag("locl"), feature_tag("rlig"), feature_tag("liga"), feature_tag("clig") };
    m_typeface->substitute(glyphs, { features, array_size(features) });
    MUST(run.glyphs.try_ensure_capacity(glyphs.size()));
    float pen = 0;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        run.glyphs.append(PositionedGlyph { glyphs[i], pen });
        int advance = m_typeface->advance_of(glyphs[i]);
        if (i + 1 < glyphs.size())
            advance += m_typeface->kerning(glyphs[i], glyphs[i + 1]);
        pen += static_cast<float>(advance) * m_pixels_per_unit;
    }
    run.width = pen;
    return run;
}

Font::Image const* Font::image_for(GlyphIndex glyph, u32 subpixel)
{
    u32 key = (static_cast<u32>(glyph) << 2) | subpixel;
    if (auto const* known = m_images.get(key))
        return known;

    // Down from font units (y up) to pixels (y down), shifted within the pixel.
    float shift = static_cast<float>(subpixel) / static_cast<float>(subpixel_positions);
    auto outline = m_typeface->outline_of(glyph).transformed(Gfx::AffineTransform(m_pixels_per_unit, 0, 0, -m_pixels_per_unit, shift, 0));
    Image image;
    if (!outline.is_empty()) {
        auto bounds = outline.bounds().enclosing_int_rect();
        if (!bounds.is_empty()) {
            u8* writable = nullptr;
            int pitch = 0;
            auto slot = m_atlas.allocate(bounds.size(), writable, pitch);
            if (!slot.is_error()) {
                image.slot = slot.value();
                image.left = bounds.x;
                image.top = bounds.y;
                Gfx::Rasterizer rasterizer(bounds);
                rasterizer.add_path(outline);
                rasterizer.rasterize(Gfx::FillRule::NonZero, [&](int y, int x, int count, u8 const* coverage) {
                    __builtin_memcpy(writable + static_cast<ptrdiff_t>(y - bounds.y) * pitch + (x - bounds.x), coverage, static_cast<size_t>(count));
                });
            }
        }
    }
    MUST(m_images.try_set(key, image));
    return m_images.get(key);
}

void Font::draw(Gfx::Painter& painter, Gfx::FloatPoint origin, ShapedRun const& run, Gfx::Color color)
{
    int baseline = static_cast<int>(Math::round(origin.y));
    for (auto const& placed : run.glyphs) {
        float x = origin.x + placed.x;
        float whole = Math::floor(x);
        auto subpixel = static_cast<u32>((x - whole) * static_cast<float>(subpixel_positions) + 0.5f);
        int pen = static_cast<int>(whole);
        if (subpixel == subpixel_positions) {
            subpixel = 0;
            ++pen;
        }
        auto const* image = image_for(placed.glyph, subpixel);
        if (!image || !image->slot.page)
            continue;
        auto const& area = image->slot.area;
        auto const* page = image->slot.page;
        painter.draw_mask(page->scanline(area.y) + area.x, page->width(), { pen + image->left, baseline + image->top, area.width, area.height }, color);
    }
}

void Font::draw(Gfx::Painter& painter, Gfx::FloatPoint origin, StringView text, Gfx::Color color)
{
    draw(painter, origin, shape(text), color);
}

}
