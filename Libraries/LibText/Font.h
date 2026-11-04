/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/HashMap.h>
#include <LibBase/RefPtr.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Painter.h>
#include <LibText/Typeface.h>

// Text for the screen: a typeface at a size, shaped into positioned glyphs
// (ligatures and kerning), rendered without hinting, as macOS does, and cached.

namespace Text {

struct PositionedGlyph {
    GlyphIndex glyph { 0 };
    // From the start of the run, in device pixels.
    float x { 0 };
    // Below the baseline, in device pixels. Nearly always none: marks on letters, and Arabic that runs downhill.
    float y { 0 };
    // Which font it's in, if not the one that shaped it.
    Typeface const* typeface { nullptr };
    // Where in the text it came from, in bytes. Only known for text that's gone through the whole shaper.
    u32 cluster { 0 };
    float advance { 0 };
    bool is_right_to_left { false };
};

struct ShapedRun {
    Vector<PositionedGlyph> glyphs;
    // Its total advance, in device pixels.
    float width { 0 };
    // Whether it went through bidi, script runs and fallback. Most of what a system says doesn't need to.
    bool is_complex { false };
};

// Where glyph images live: pages of coverage values, packed in shelves.
class GlyphAtlas {
public:
    struct Slot {
        Gfx::AlphaMask const* page { nullptr };
        Gfx::IntRect area;
    };

    // Room for an image this size; empty if it can never fit.
    [[nodiscard]] ErrorOr<Slot> allocate(Gfx::IntSize, u8*& writable, int& pitch);
    [[nodiscard]] size_t page_count() const { return m_pages.size(); }

private:
    static constexpr int page_size = 1024;

    struct Page {
        NonnullRefPtr<Gfx::AlphaMask> mask;
        int shelf_x { 0 };
        int shelf_y { 0 };
        int shelf_height { 0 };
    };

    Vector<Page> m_pages;
};

// A typeface at a size in points, for a display of some scale (2 on a 2x one).
// One thread at a time: a window server, or a client, has its own per thread.
class Font {
public:
    Font(NonnullRefPtr<Typeface>, float points, float scale, GlyphAtlas&);

    [[nodiscard]] Typeface const& typeface() const { return *m_typeface; }
    // Slanted, for a font with no italics of its own: which is what a Mac does too.
    void set_oblique(bool oblique) { m_oblique = oblique; }
    [[nodiscard]] float points() const { return m_points; }
    [[nodiscard]] float scale() const { return m_scale; }
    // Of the typeface, in device pixels.
    [[nodiscard]] float ascent() const;
    [[nodiscard]] float descent() const;
    [[nodiscard]] float line_height() const;
    [[nodiscard]] float cap_height() const;
    [[nodiscard]] float x_height() const;

    // UTF-8 (bad sequences show as .notdef) to glyphs and positions.
    [[nodiscard]] ShapedRun shape(StringView text) const;
    [[nodiscard]] float width_of(StringView text) const { return shape(text).width; }

    // Where a caret goes for this place in the text (in bytes), from the start of the line, in device pixels: on either side of what's there, according to which way it's going.
    [[nodiscard]] float x_of_offset(StringView text, size_t offset) const;
    // The place in the text nearest this.
    [[nodiscard]] size_t offset_at_x(StringView text, float x) const;

    // Draws with the baseline starting at `origin`, in device pixels (the
    // painter's transform is not applied, as glyphs are made for one size).
    void draw(Gfx::Painter&, Gfx::FloatPoint origin, ShapedRun const&, Gfx::Color);
    void draw(Gfx::Painter&, Gfx::FloatPoint origin, StringView text, Gfx::Color);

private:
    // Glyphs can sit at quarters of a pixel across (none vertically: baselines
    // stay on the pixel grid, as they do on macOS).
    static constexpr u32 subpixel_positions = 4;

    struct Image {
        GlyphAtlas::Slot slot;
        // From the pen position to the image's top-left corner.
        int left { 0 };
        int top { 0 };
    };

    [[nodiscard]] Image const* image_for(Typeface const&, GlyphIndex, u32 subpixel);
    [[nodiscard]] ShapedRun shape_complex(StringView text) const;
    [[nodiscard]] Gfx::Bitmap const* color_bitmap_for(Typeface const&, GlyphIndex);

    NonnullRefPtr<Typeface> m_typeface;
    float m_points;
    float m_scale;
    float m_pixels_per_unit;
    bool m_oblique { false };
    GlyphAtlas& m_atlas;
    HashMap<u64, Image> m_images;
    HashMap<u64, RefPtr<Gfx::Bitmap>> m_color_bitmaps;
};

}
