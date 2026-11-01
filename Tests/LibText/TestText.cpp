/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibBase/String.h>
#include <LibGfx/Rasterizer.h>
#include <LibTest/TestCase.h>
#include <LibText/Font.h>

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
}

using namespace Text;
namespace Math = Base::Math;

namespace {

Vector<u8> read_file(StringView directory, StringView name)
{
    StringBuilder path;
    path.append(directory);
    path.append(name);
    auto full_path = MUST(path.to_string());
    FILE* file = fopen(full_path.characters(), "rb");
    VERIFY(file);
    Vector<u8> bytes;
    u8 chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        size_t at = bytes.size();
        MUST(bytes.try_resize(at + got));
        __builtin_memcpy(bytes.data() + at, chunk, got);
    }
    fclose(file);
    return bytes;
}

NonnullRefPtr<Typeface> system_font(StringView name)
{
    return MUST(Typeface::parse(read_file(StringView(VULTURE_FONT_DIRECTORY), name)));
}

// The coverage of a glyph at a size, on a grid with the origin at (8, height - 8).
struct Coverage {
    int width;
    int height;
    Vector<u8> cells;
    [[nodiscard]] double area() const
    {
        double total = 0;
        for (u8 cell : cells)
            total += cell / 255.0;
        return total;
    }
};

Coverage rasterize(Typeface const& typeface, GlyphIndex glyph, float pixels)
{
    float scale = pixels / static_cast<float>(typeface.units_per_em());
    int width = static_cast<int>(pixels * 2), height = static_cast<int>(pixels * 2);
    Coverage result { width, height, {} };
    MUST(result.cells.try_resize(static_cast<size_t>(width * height)));
    __builtin_memset(result.cells.data(), 0, result.cells.size());
    auto outline = typeface.outline_of(glyph).transformed(Gfx::AffineTransform(scale, 0, 0, -scale, 8, static_cast<float>(height) - 8));
    Gfx::Rasterizer rasterizer({ 0, 0, width, height });
    rasterizer.add_path(outline);
    rasterizer.rasterize(Gfx::FillRule::NonZero, [&](int y, int x, int count, u8 const* coverage) {
        for (int i = 0; i < count; ++i)
            result.cells[static_cast<size_t>(y * width + x + i)] = coverage[i];
    });
    return result;
}

}

TEST_CASE(reads_the_font_tables)
{
    auto inter = system_font("Inter-Regular.ttf"_sv);
    EXPECT(inter->family_name().view() == "Inter"_sv);
    EXPECT(inter->style_name().view() == "Regular"_sv);
    EXPECT_EQ(inter->units_per_em(), 2048);
    EXPECT_EQ(inter->weight(), 400);
    EXPECT(inter->ascent() > 1500 && inter->descent() < -300);
    EXPECT(inter->cap_height() > 1400 && inter->cap_height() < 1600);
    EXPECT(inter->x_height() > 1000 && inter->x_height() < inter->cap_height());
    EXPECT(inter->glyph_count() > 2000);

    auto bold = system_font("Inter-Bold.ttf"_sv);
    EXPECT_EQ(bold->weight(), 700);
    EXPECT(bold->style_name().view() == "Bold"_sv);
    EXPECT(inter->id() != bold->id());

    auto mono = system_font("JetBrainsMono-Regular.ttf"_sv);
    EXPECT(mono->family_name().view() == "JetBrains Mono"_sv);
    // A fixed pitch: i and M alike.
    auto advance = mono->advance_of(mono->glyph_for_code_point('i'));
    EXPECT_EQ(mono->advance_of(mono->glyph_for_code_point('M')), advance);
    EXPECT(advance > 0);
}

TEST_CASE(character_map)
{
    auto inter = system_font("Inter-Regular.ttf"_sv);
    auto a = inter->glyph_for_code_point('A');
    auto b = inter->glyph_for_code_point('B');
    EXPECT(a != 0 && b != 0 && a != b);
    EXPECT(inter->glyph_for_code_point(0x00e9) != 0); // é
    EXPECT(inter->glyph_for_code_point(0x20ac) != 0); // €
    EXPECT(inter->glyph_for_code_point(0x10ffff) == 0);
    EXPECT(inter->glyph_for_code_point(0xe0123) == 0);
    EXPECT(inter->advance_of(a) > 0);
    EXPECT(inter->advance_of(inter->glyph_for_code_point(' ')) > 0);
    EXPECT(inter->outline_of(inter->glyph_for_code_point(' ')).is_empty());
}

TEST_CASE(outlines)
{
    auto inter = system_font("Inter-Regular.ttf"_sv);
    auto bounds_of = [&](u32 code_point) { return inter->outline_of(inter->glyph_for_code_point(code_point)).bounds(); };
    // H stands on the baseline to the cap height.
    auto h = bounds_of('H');
    EXPECT(Math::absolute(h.y) < 1);
    EXPECT(Math::absolute(h.bottom() - static_cast<float>(inter->cap_height())) < 2);
    // g has a descender; x is x-height tall.
    EXPECT(bounds_of('g').y < -100);
    EXPECT(Math::absolute(bounds_of('x').bottom() - static_cast<float>(inter->x_height())) < 2);
    // é is e with something on top, a composite: same width of bowl, taller.
    auto e = bounds_of('e');
    auto accented = bounds_of(0x00e9);
    EXPECT(accented.bottom() > e.bottom() + 100);
    EXPECT(Math::absolute(accented.x - e.x) < 2);

    // Counters are holes: O is much less than its bounding oval.
    auto o = rasterize(*inter, inter->glyph_for_code_point('O'), 64);
    EXPECT(o.area() > 200 && o.area() < 1400);
    auto bold = system_font("Inter-Bold.ttf"_sv);
    auto heavy = rasterize(*bold, bold->glyph_for_code_point('O'), 64);
    EXPECT(heavy.area() > o.area() * 1.3);
    // Far beyond the glyph count: nothing, no trouble.
    EXPECT(inter->outline_of(65535).is_empty());
}

// The same design in TrueType and in CFF: nearly the same pixels. This is what
// checks the charstring interpreter.
TEST_CASE(cff_outlines_match_truetype)
{
    auto truetype = system_font("Inter-Regular.ttf"_sv);
    auto cff = MUST(Typeface::parse(read_file(StringView(VULTURE_TEXT_TEST_DATA), "Inter-Regular.otf"_sv)));
    EXPECT(cff->family_name().view() == "Inter"_sv);
    EXPECT_EQ(cff->units_per_em(), truetype->units_per_em());
    EXPECT_EQ(cff->glyph_count(), truetype->glyph_count());
    constexpr StringView sample = "AaBbGgOoQqRrSs&@%?8 3éçÅñ"_sv;
    for (size_t at = 0; at < sample.length(); ++at) {
        u32 code_point = static_cast<u8>(sample[at]);
        if (code_point >= 0x80) {
            // Two-byte characters in this sample.
            code_point = ((code_point & 0x1f) << 6) | (static_cast<u8>(sample[++at]) & 0x3f);
        }
        auto a = truetype->glyph_for_code_point(code_point);
        auto b = cff->glyph_for_code_point(code_point);
        EXPECT(a != 0 && b != 0);
        auto first = rasterize(*truetype, a, 48);
        auto second = rasterize(*cff, b, 48);
        EXPECT(Math::absolute(first.area() - second.area()) <= first.area() * 0.02 + 1);
        double differing = 0;
        for (size_t i = 0; i < first.cells.size(); ++i)
            differing += Math::absolute(static_cast<int>(first.cells[i]) - static_cast<int>(second.cells[i])) / 255.0;
        EXPECT(differing < first.area() * 0.06 + 2);
    }
}

TEST_CASE(kerning)
{
    auto inter = system_font("Inter-Regular.ttf"_sv);
    auto glyph = [&](char c) { return inter->glyph_for_code_point(static_cast<u8>(c)); };
    // Well-known pairs tuck in; plain ones do nothing.
    EXPECT(inter->kerning(glyph('A'), glyph('V')) < -20);
    EXPECT(inter->kerning(glyph('T'), glyph('o')) < -20);
    EXPECT(inter->kerning(glyph('H'), glyph('H')) == 0);
    // The cache gives the same answer.
    EXPECT_EQ(inter->kerning(glyph('A'), glyph('V')), inter->kerning(glyph('A'), glyph('V')));

    GlyphAtlas atlas;
    Font font(inter, 13, 2, atlas);
    auto kerned = font.shape("AV"_sv);
    float plain = static_cast<float>(inter->advance_of(glyph('A'))) * 26 / 2048;
    EXPECT(kerned.glyphs[1].x < plain);
    EXPECT_EQ(kerned.glyphs.size(), 2u);
}

TEST_CASE(substitutions)
{
    auto mono = system_font("JetBrainsMono-Regular.ttf"_sv);
    Vector<GlyphIndex> glyphs;
    auto arrow_start = mono->glyph_for_code_point('-');
    auto arrow_end = mono->glyph_for_code_point('>');
    glyphs.append(arrow_start);
    glyphs.append(arrow_end);
    // Contextual alternates only when asked for: JetBrains Mono's ligatures.
    auto untouched = glyphs;
    u32 const liga[] = { feature_tag("liga") };
    mono->substitute(untouched, { liga, 1 });
    EXPECT(untouched[0] == arrow_start && untouched[1] == arrow_end);
    u32 const calt[] = { feature_tag("calt") };
    auto changed = glyphs;
    mono->substitute(changed, { calt, 1 });
    EXPECT(changed.size() == 2 && (changed[0] != arrow_start || changed[1] != arrow_end));

    // Inter's localized and composed forms do no harm to plain text.
    auto inter = system_font("Inter-Regular.ttf"_sv);
    GlyphAtlas atlas;
    Font font(inter, 13, 1, atlas);
    EXPECT_EQ(font.shape("Hello, world"_sv).glyphs.size(), 12u);
    // UTF-8: é is one glyph; a broken sequence is shown as something.
    EXPECT_EQ(font.shape("caf\xc3\xa9"_sv).glyphs.size(), 4u);
    EXPECT_EQ(font.shape("a\xff"_sv).glyphs.size(), 2u);
    EXPECT_EQ(font.shape("\xe2\x82"_sv).glyphs.size(), 1u);
    EXPECT_EQ(font.shape(""_sv).glyphs.size(), 0u);
    EXPECT(font.width_of(""_sv) == 0);
}

TEST_CASE(font_metrics_scale)
{
    auto inter = system_font("Inter-Regular.ttf"_sv);
    GlyphAtlas atlas;
    Font normal(inter, 13, 1, atlas);
    Font retina(inter, 13, 2, atlas);
    EXPECT(Math::absolute(retina.ascent() - normal.ascent() * 2) < 0.01f);
    EXPECT(Math::absolute(retina.line_height() - normal.line_height() * 2) < 0.01f);
    EXPECT(Math::absolute(retina.width_of("Sequoia"_sv) - normal.width_of("Sequoia"_sv) * 2) < 0.01f);
    EXPECT(normal.line_height() > 14 && normal.line_height() < 18);
    EXPECT(normal.width_of("Hello"_sv) > 25 && normal.width_of("Hello"_sv) < 40);
}

TEST_CASE(drawing_text)
{
    auto inter = system_font("Inter-Regular.ttf"_sv);
    GlyphAtlas atlas;
    Font font(inter, 13, 2, atlas);
    auto bitmap = MUST(Gfx::Bitmap::create({ 200, 40 }));
    bitmap->clear(Gfx::Color::from_rgb(0xffffff));
    Gfx::Painter painter(*bitmap);
    font.draw(painter, { 4, 30 }, "Hamburgefonstiv"_sv, Gfx::Color::from_rgb(0));
    // Ink where the text is, none above its top or left of its start.
    int ink = 0;
    int leftmost = 200;
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 200; ++x) {
            if (bitmap->color_at(x, y).red < 128) {
                ++ink;
                leftmost = min(leftmost, x);
            }
        }
    }
    EXPECT(ink > 400 && ink < 3000);
    EXPECT(leftmost >= 4 && leftmost < 10);
    for (int x = 0; x < 200; ++x)
        EXPECT_EQ(bitmap->color_at(x, 2).red, 255);
    EXPECT(atlas.page_count() == 1);

    // Drawn again, it is the same, from the cache.
    auto again = MUST(Gfx::Bitmap::create({ 200, 40 }));
    again->clear(Gfx::Color::from_rgb(0xffffff));
    Gfx::Painter second(*again);
    font.draw(second, { 4, 30 }, "Hamburgefonstiv"_sv, Gfx::Color::from_rgb(0));
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 200; ++x)
            EXPECT_EQ(again->pixel_at(x, y), bitmap->pixel_at(x, y));
    }

    // A quarter pixel to the right shows: not snapped to whole pixels.
    auto shifted = MUST(Gfx::Bitmap::create({ 200, 40 }));
    shifted->clear(Gfx::Color::from_rgb(0xffffff));
    Gfx::Painter third(*shifted);
    font.draw(third, { 4.25f, 30 }, "Hamburgefonstiv"_sv, Gfx::Color::from_rgb(0));
    int changed = 0;
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 200; ++x)
            changed += shifted->pixel_at(x, y) != bitmap->pixel_at(x, y);
    }
    EXPECT(changed > 200);
}

TEST_CASE(atlas_grows)
{
    auto inter = system_font("Inter-Regular.ttf"_sv);
    GlyphAtlas atlas;
    Font big(inter, 200, 2, atlas);
    auto bitmap = MUST(Gfx::Bitmap::create({ 16, 16 }));
    Gfx::Painter painter(*bitmap);
    // Big glyphs fill pages; each remains good.
    for (char c = 'A'; c <= 'Z'; ++c) {
        char text[2] = { c, 0 };
        big.draw(painter, { 0, 0 }, StringView(text), Gfx::Color::from_rgb(0));
    }
    EXPECT(atlas.page_count() > 1);
    // Too big for any page: skipped, not a failure.
    Font enormous(inter, 2000, 2, atlas);
    enormous.draw(painter, { 0, 0 }, "W"_sv, Gfx::Color::from_rgb(0));
}

TEST_CASE(damaged_fonts_do_no_harm)
{
    EXPECT(Typeface::parse(Vector<u8> {}).is_error());
    Vector<u8> junk;
    for (int i = 0; i < 100; ++i)
        junk.append(static_cast<u8>(i));
    EXPECT(Typeface::parse(junk).is_error());

    auto file = read_file(StringView(VULTURE_FONT_DIRECTORY), "Inter-Regular.ttf"_sv);
    auto cff_file = read_file(StringView(VULTURE_TEXT_TEST_DATA), "Inter-Regular.otf"_sv);
    u32 state = 12345;
    auto next = [&] {
        state = state * 1103515245 + 12345;
        return state >> 8;
    };
    auto exercise = [&](Vector<u8> damaged) {
        auto typeface = Typeface::parse(move(damaged));
        if (typeface.is_error())
            return;
        auto const& font = *typeface.value();
        u32 const sample[] = { 65u, 86u, 103u, 233u, 8364u };
        for (u32 c : sample) {
            auto glyph = font.glyph_for_code_point(c);
            (void)font.outline_of(glyph);
            (void)font.advance_of(glyph);
            (void)font.kerning(glyph, 65);
        }
        (void)font.outline_of(static_cast<GlyphIndex>(next()));
        Vector<GlyphIndex> glyphs;
        glyphs.append(font.glyph_for_code_point('f'));
        glyphs.append(font.glyph_for_code_point('i'));
        u32 const features[] = { feature_tag("liga"), feature_tag("calt"), feature_tag("ccmp") };
        font.substitute(glyphs, { features, 3 });
    };
    for (int round = 0; round < 60; ++round) {
        auto damaged = round % 2 ? file : cff_file;
        // A few bytes anywhere, and some in the table directory.
        for (int i = 0; i < 40; ++i)
            damaged[next() % damaged.size()] = static_cast<u8>(next());
        for (int i = 0; i < 4; ++i)
            damaged[next() % 300] = static_cast<u8>(next());
        exercise(move(damaged));
    }
}
