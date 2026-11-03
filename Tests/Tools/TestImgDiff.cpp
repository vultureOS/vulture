/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Compare.h>
#include <LibImage/Image.h>
#include <LibTest/TestCase.h>

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
}

using namespace ImgDiff;

namespace {

NonnullRefPtr<Gfx::Bitmap> plain(int width, int height, Gfx::Pixel pixel = 0xff808080)
{
    auto bitmap = MUST(Gfx::Bitmap::create({ width, height }));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x)
            bitmap->set_pixel_at(x, y, pixel);
    }
    return bitmap;
}

NonnullRefPtr<Gfx::Bitmap> copy_of(Gfx::Bitmap const& bitmap)
{
    auto copy = MUST(Gfx::Bitmap::create(bitmap.size()));
    for (int y = 0; y < bitmap.height(); ++y) {
        for (int x = 0; x < bitmap.width(); ++x)
            copy->set_pixel_at(x, y, bitmap.pixel_at(x, y));
    }
    return copy;
}

// What a colour tag's dot looks like when it's lost: a disc of it gone back to what's behind it.
void remove_dot(Gfx::Bitmap& bitmap, int center_x, int center_y, int radius, Gfx::Pixel behind)
{
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius)
                bitmap.set_pixel_at(center_x + x, center_y + y, behind);
        }
    }
}

NonnullRefPtr<Gfx::Bitmap> golden(char const* path)
{
    FILE* file = fopen(path, "rb");
    VERIFY(file);
    Vector<u8> bytes;
    u8 chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        size_t const at = bytes.size();
        MUST(bytes.try_resize(at + got));
        __builtin_memcpy(bytes.data() + at, chunk, got);
    }
    fclose(file);
    return MUST(Image::decode(bytes.span()));
}

}

TEST_CASE(identical_pictures_match)
{
    auto a = plain(40, 30);
    auto b = plain(40, 30);
    auto const result = compare(*a, *b, {});
    EXPECT(result.matches());
    EXPECT_EQ(result.off, 0u);
    EXPECT_EQ(result.largest_cluster, 0u);
}

TEST_CASE(a_channel_off_by_the_tolerance_is_not_off)
{
    auto a = plain(40, 30, 0xff808080);
    auto b = plain(40, 30, 0xff828282);
    EXPECT_EQ(compare(*a, *b, {}).off, 0u);
    auto c = plain(40, 30, 0xff838383);
    EXPECT_EQ(compare(*a, *c, {}).off, 1200u);
}

TEST_CASE(scattered_pixels_within_the_share_match)
{
    auto a = plain(1000, 1000);
    auto b = copy_of(*a);
    // 1,000 of a million: 0.1%, and no two touching.
    for (int i = 0; i < 1000; ++i)
        b->set_pixel_at((i % 40) * 25, (i / 40) * 25, 0xffffffff);
    auto const result = compare(*a, *b, {});
    EXPECT_EQ(result.off, 1000u);
    EXPECT_EQ(result.largest_cluster, 1u);
    EXPECT(result.matches());
}

TEST_CASE(scattered_pixels_beyond_the_share_do_not)
{
    auto a = plain(100, 100);
    auto b = copy_of(*a);
    for (int i = 0; i < 25; ++i)
        b->set_pixel_at((i % 5) * 20, (i / 5) * 20, 0xffffffff);
    auto const result = compare(*a, *b, {});
    EXPECT(!result.scatter_ok);
    EXPECT(result.cluster_ok);
    EXPECT(!result.matches());
}

TEST_CASE(a_lump_is_caught_though_the_share_would_pass)
{
    auto a = plain(2560, 1600);
    auto b = copy_of(*a);
    remove_dot(*b, 500, 500, 10, 0xff000000);
    auto const result = compare(*a, *b, {});
    EXPECT(result.off < result.allowed);
    EXPECT(result.scatter_ok);
    EXPECT(!result.cluster_ok);
    EXPECT(result.largest_cluster > 300u);
    EXPECT(!result.matches());
}

TEST_CASE(pixels_touching_at_a_corner_are_one_group)
{
    auto a = plain(40, 40);
    auto b = copy_of(*a);
    for (int i = 0; i < 10; ++i)
        b->set_pixel_at(i + 5, i + 5, 0xffffffff);
    EXPECT_EQ(compare(*a, *b, {}).largest_cluster, 10u);
}

TEST_CASE(a_strict_region_allows_nothing)
{
    auto a = plain(100, 100);
    auto b = copy_of(*a);
    b->set_pixel_at(15, 15, 0xffffffff);

    Options options;
    options.max_percent = 100;
    Region region;
    region.kind = RegionKind::Strict;
    region.x = 10;
    region.y = 10;
    region.width = 10;
    region.height = 10;
    region.name = MUST(String::from_view("the dot"_sv));
    options.regions.append(move(region));

    auto const result = compare(*a, *b, options);
    EXPECT(result.scatter_ok);
    EXPECT(result.cluster_ok);
    EXPECT_EQ(result.strict_failures.size(), 1u);
    EXPECT_EQ(result.strict_failures[0].off, 1u);
    EXPECT_EQ(result.strict_failures[0].first_x, 15);
    EXPECT(!result.matches());
}

TEST_CASE(a_strict_region_says_nothing_about_elsewhere)
{
    auto a = plain(100, 100);
    auto b = copy_of(*a);
    b->set_pixel_at(50, 50, 0xffffffff);

    Options options;
    options.max_percent = 100;
    Region region;
    region.x = 10;
    region.y = 10;
    region.width = 10;
    region.height = 10;
    options.regions.append(move(region));
    EXPECT(compare(*a, *b, options).matches());
}

TEST_CASE(an_ignored_region_counts_for_nothing)
{
    auto a = plain(200, 200);
    auto b = copy_of(*a);
    // A clock, say: a big change, but nobody's asking.
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 60; ++x)
            b->set_pixel_at(x, y, 0xffffffff);
    }
    EXPECT(!compare(*a, *b, {}).matches());

    Options options;
    Region region;
    region.kind = RegionKind::Ignore;
    region.width = 60;
    region.height = 20;
    options.regions.append(move(region));
    auto const result = compare(*a, *b, options);
    EXPECT(result.matches());
    EXPECT_EQ(result.off, 0u);
    EXPECT_EQ(result.worst, 0);
}

TEST_CASE(what_the_difference_picture_shows)
{
    auto a = plain(10, 10);
    auto b = copy_of(*a);
    b->set_pixel_at(3, 3, 0xffffffff);
    auto difference = MUST(Gfx::Bitmap::create(a->size()));
    (void)compare(*a, *b, {}, difference.ptr());
    EXPECT(Gfx::pixel_red(difference->pixel_at(3, 3)) == 255);
    EXPECT(Gfx::pixel_red(difference->pixel_at(4, 4)) == 0);
}

TEST_CASE(region_files)
{
    auto const regions = MUST(parse_regions(R"(
# A comment, then a blank line.

strict 10 20 30 40 the tag's dot
ignore 0 0 100 12 the menu bar clock
)"_sv));
    EXPECT_EQ(regions.size(), 2u);
    EXPECT(regions[0].kind == RegionKind::Strict);
    EXPECT_EQ(regions[0].x, 10);
    EXPECT_EQ(regions[0].height, 40);
    EXPECT(regions[0].name.view() == "the tag's dot"_sv);
    EXPECT(regions[1].kind == RegionKind::Ignore);
    EXPECT(regions[1].name.view() == "the menu bar clock"_sv);

    EXPECT(parse_regions("sometimes 1 2 3 4"_sv).is_error());
    EXPECT(parse_regions("strict 1 2 3"_sv).is_error());
    EXPECT(parse_regions("strict 1 2 0 4"_sv).is_error());
    EXPECT(parse_regions("strict -1 2 3 4"_sv).is_error());
}

// What this is all for: on a real screenshot, the old rule alone calls this a match.
TEST_CASE(a_dot_gone_from_a_real_golden_is_noticed)
{
    auto const before = golden(GOLDEN_DIRECTORY "/desktop-2x-desktop.png");
    auto after = copy_of(*before);
    // At 2x, a colour tag's dot is about 14 pixels across. Somewhere in the picture's middle, in a colour that is surely not there.
    remove_dot(*after, before->width() / 2, before->height() / 2, 7, 0xffff00ff);

    Options old_rule;
    old_rule.max_cluster = -1;
    EXPECT(compare(*before, *after, old_rule).matches());

    EXPECT(!compare(*before, *after, {}).matches());

    Options with_region;
    with_region.max_cluster = -1;
    Region region;
    region.x = before->width() / 2 - 10;
    region.y = before->height() / 2 - 10;
    region.width = 20;
    region.height = 20;
    region.name = MUST(String::from_view("the tag"_sv));
    with_region.regions.append(move(region));
    auto const result = compare(*before, *after, with_region);
    EXPECT_EQ(result.strict_failures.size(), 1u);
    EXPECT(!result.matches());
}
