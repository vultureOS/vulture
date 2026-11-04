/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibGfx/Rasterizer.h>
#include <LibImage/Image.h>
#include <LibText/Font.h>
#include <LibText/Itemizer.h>
#include <LibText/Shaper.h>

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

namespace {

// Whether this font can just set this text one glyph after another, with nothing going on that needs anything more. That's nearly all of what a system says, and it's kept apart so that
// it costs what it always has.
bool is_plain_text(Typeface const& typeface, StringView text)
{
    for (size_t at = 0; at < text.length();) {
        if (static_cast<u8>(text[at]) < 0x80) {
            ++at;
            continue;
        }
        u32 const code = next_code_point(text, at);
        if (code == 0xfffd)
            continue;
        if (typeface.glyph_for_code_point(code) == 0)
            return false;
        auto const category = Unicode::general_category(code);
        if (category == Unicode::GeneralCategory::Mn || category == Unicode::GeneralCategory::Mc || category == Unicode::GeneralCategory::Me || category == Unicode::GeneralCategory::Cf)
            return false;
        auto const direction = Unicode::bidi_class_of(code);
        if (direction == Unicode::BidiClass::R || direction == Unicode::BidiClass::AL || direction == Unicode::BidiClass::AN)
            return false;
        if (Unicode::is_emoji_presentation(code))
            return false;
    }
    return true;
}

}

ShapedRun Font::shape_complex(StringView text) const
{
    ShapedRun run;
    run.is_complex = true;
    // Characters, and which byte each starts at.
    Vector<Unicode::CodePoint> points;
    Vector<u32> byte_offsets;
    for (size_t at = 0; at < text.length();) {
        MUST(byte_offsets.try_append(static_cast<u32>(at)));
        MUST(points.try_append(next_code_point(text, at)));
    }
    auto const items = itemize(points.span(), *m_typeface);
    if (items.is_error())
        return run;
    float pen = 0;
    for (auto const& item : items.value()) {
        auto const& typeface = *item.typeface;
        float const scale = m_points * m_scale / static_cast<float>(typeface.units_per_em());
        ShapeRequest request;
        request.text = points.span().slice(item.start, item.end - item.start);
        request.script = item.script;
        request.is_right_to_left = item.is_right_to_left;
        auto shaped = Text::shape(typeface, request);
        if (shaped.is_error())
            continue;
        for (auto const& glyph : shaped.value()) {
            PositionedGlyph placed;
            placed.glyph = glyph.glyph;
            placed.x = pen + static_cast<float>(glyph.x_offset) * scale;
            placed.y = -static_cast<float>(glyph.y_offset) * scale;
            placed.typeface = &typeface == m_typeface.ptr() ? nullptr : &typeface;
            placed.advance = static_cast<float>(glyph.x_advance) * scale;
            placed.is_right_to_left = item.is_right_to_left;
            size_t const point = item.start + glyph.cluster;
            placed.cluster = point < byte_offsets.size() ? byte_offsets[point] : static_cast<u32>(text.length());
            MUST(run.glyphs.try_append(placed));
            pen += placed.advance;
        }
    }
    run.width = pen;
    return run;
}

float Font::x_of_offset(StringView text, size_t offset) const
{
    if (is_plain_text(*m_typeface, text))
        return width_of(text.substring_view(0, min(offset, text.length())));
    auto const run = shape_complex(text);
    if (run.glyphs.is_empty())
        return 0;
    // The pen before a glyph isn't quite its x, as x has its offset in it. For a caret, that doesn't matter: what's asked for is where its cluster begins.
    float pen = 0;
    Optional<float> at_or_after;
    u32 nearest = 0xffffffff;
    float end_of_last = 0;
    u32 last_cluster = 0;
    for (auto const& glyph : run.glyphs) {
        float const left = pen;
        float const right = pen + glyph.advance;
        pen = right;
        // Going by the start of the first cluster that's at this place or past it, on whichever side that starts.
        if (glyph.cluster >= offset && glyph.cluster < nearest) {
            nearest = glyph.cluster;
            at_or_after = glyph.is_right_to_left ? right : left;
        } else if (glyph.cluster == nearest && glyph.is_right_to_left) {
            at_or_after = max(at_or_after.value(), right);
        }
        if (glyph.cluster >= last_cluster) {
            last_cluster = glyph.cluster;
            end_of_last = glyph.is_right_to_left ? left : right;
        }
    }
    if (at_or_after.has_value())
        return at_or_after.value();
    return end_of_last;
}

size_t Font::offset_at_x(StringView text, float x) const
{
    if (is_plain_text(*m_typeface, text)) {
        size_t best = 0;
        float best_distance = x < 0 ? -x : x;
        for (size_t at = 0; at < text.length();) {
            (void)next_code_point(text, at);
            float distance = width_of(text.substring_view(0, at)) - x;
            if (distance < 0)
                distance = -distance;
            if (distance < best_distance) {
                best = at;
                best_distance = distance;
            }
        }
        return best;
    }
    auto const run = shape_complex(text);
    float pen = 0;
    for (size_t i = 0; i < run.glyphs.size(); ++i) {
        auto const& glyph = run.glyphs[i];
        float const right = pen + glyph.advance;
        // A mark has no width: what it's on is what's hit.
        if (x < right) {
            bool const first_half = x < pen + glyph.advance / 2;
            if (first_half != glyph.is_right_to_left)
                return glyph.cluster;
            // The far side of it: the next cluster, in the text's own order.
            u32 next = static_cast<u32>(text.length());
            for (auto const& other : run.glyphs) {
                if (other.cluster > glyph.cluster && other.cluster < next)
                    next = other.cluster;
            }
            return next;
        }
        pen = right;
    }
    // Past the end, which is one end of the text or the other.
    return !run.glyphs.is_empty() && run.glyphs.last().is_right_to_left ? 0 : text.length();
}

ShapedRun Font::shape(StringView text) const
{
    if (!is_plain_text(*m_typeface, text))
        return shape_complex(text);
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

// About twelve degrees.
constexpr float oblique_slant = 0.2126f;

Font::Image const* Font::image_for(Typeface const& typeface, GlyphIndex glyph, u32 subpixel)
{
    u64 const key = (static_cast<u64>(typeface.id()) << 32) | (static_cast<u64>(glyph) << 2) | subpixel;
    if (auto const* known = m_images.get(key))
        return known;

    // Down from font units (y up) to pixels (y down), shifted within the pixel.
    float shift = static_cast<float>(subpixel) / static_cast<float>(subpixel_positions);
    float const pixels_per_unit = m_points * m_scale / static_cast<float>(typeface.units_per_em());
    auto outline = typeface.outline_of(glyph).transformed(Gfx::AffineTransform(pixels_per_unit, 0, m_oblique ? pixels_per_unit * oblique_slant : 0, -pixels_per_unit, shift, 0));
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

Gfx::Bitmap const* Font::color_bitmap_for(Typeface const& typeface, GlyphIndex glyph)
{
    u64 const key = (static_cast<u64>(typeface.id()) << 32) | glyph;
    if (auto const* known = m_color_bitmaps.get(key))
        return known->ptr();
    RefPtr<Gfx::Bitmap> picture;
    if (auto const strike = typeface.color_bitmap(glyph, static_cast<u16>(m_points * m_scale + 0.5f)); strike.has_value()) {
        if (auto decoded = ::Image::decode_png(strike->png); !decoded.is_error())
            picture = decoded.release_value();
    }
    MUST(m_color_bitmaps.try_set(key, picture));
    return picture.ptr();
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
        auto const& typeface = placed.typeface ? *placed.typeface : *m_typeface;
        int const line = baseline + static_cast<int>(Math::round(placed.y));
        if (typeface.has_color_glyphs()) {
            // Shapes, each in a colour of its own.
            auto const layers = typeface.color_layers(placed.glyph);
            if (!layers.is_empty()) {
                for (auto const& layer : layers) {
                    auto const* image = image_for(typeface, layer.glyph, subpixel);
                    if (!image || !image->slot.page)
                        continue;
                    auto const& area = image->slot.area;
                    auto const* page = image->slot.page;
                    auto const shown = layer.is_text_color ? color : Gfx::Color::from_rgb(layer.argb & 0xffffff, static_cast<u8>(layer.argb >> 24));
                    painter.draw_mask(page->scanline(area.y) + area.x, page->width(), { pen + image->left, line + image->top, area.width, area.height }, shown);
                }
                continue;
            }
            // Or a picture.
            if (auto const* picture = color_bitmap_for(typeface, placed.glyph)) {
                auto const strike = typeface.color_bitmap(placed.glyph, static_cast<u16>(m_points * m_scale + 0.5f));
                float const scale = m_points * m_scale / static_cast<float>(strike->pixels_per_em);
                Gfx::FloatRect const destination {
                    x + static_cast<float>(strike->left) * scale,
                    static_cast<float>(line) - static_cast<float>(strike->top) * scale,
                    static_cast<float>(picture->width()) * scale,
                    static_cast<float>(picture->height()) * scale,
                };
                // (These are device pixels, as everything here is: a picture would otherwise go through the transform too.)
                painter.save();
                painter.set_transform({});
                painter.draw_bitmap(*picture, destination);
                painter.restore();
                continue;
            }
        }
        auto const* image = image_for(typeface, placed.glyph, subpixel);
        if (!image || !image->slot.page)
            continue;
        auto const& area = image->slot.area;
        auto const* page = image->slot.page;
        painter.draw_mask(page->scanline(area.y) + area.x, page->width(), { pen + image->left, line + image->top, area.width, area.height }, color);
    }
}

void Font::draw(Gfx::Painter& painter, Gfx::FloatPoint origin, StringView text, Gfx::Color color)
{
    draw(painter, origin, shape(text), color);
}

}
