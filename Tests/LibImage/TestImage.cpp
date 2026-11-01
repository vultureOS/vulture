/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Checksum.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibCompress/Deflate.h>
#include <LibGfx/Bitmap.h>
#include <LibImage/Image.h>
#include <LibTest/TestCase.h>

// data/ was made by ImageMagick and libjpeg's cjpeg, encoders that are not ours.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
}

using namespace Gfx;

namespace {

Vector<u8> read_data_file(StringView name)
{
    StringBuilder path;
    path.append(StringView(VULTURE_IMAGE_TEST_DATA));
    path.append(name);
    auto full_path = MUST(path.to_string());
    FILE* file = fopen(full_path.characters(), "rb");
    VERIFY(file);
    Vector<u8> bytes;
    u8 chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        size_t at = bytes.size();
        MUST(bytes.try_resize(at + got));
        __builtin_memcpy(bytes.data() + at, chunk, got);
    }
    fclose(file);
    return bytes;
}

NonnullRefPtr<Bitmap> load(StringView name)
{
    auto bytes = read_data_file(name);
    auto image = Image::decode(bytes.span());
    VERIFY(!image.is_error());
    return image.release_value();
}

// The largest difference in any channel, and the average of all of them.
struct Difference {
    int worst { 0 };
    double average { 0 };
};

Difference compare(Bitmap const& a, Bitmap const& b)
{
    Difference result;
    VERIFY(a.size() == b.size());
    u64 sum = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            auto first = a.color_at(x, y), second = b.color_at(x, y);
            auto distance = [](u8 p, u8 q) { return p > q ? p - q : q - p; };
            int red = distance(first.red, second.red), green = distance(first.green, second.green), blue = distance(first.blue, second.blue);
            sum += static_cast<u64>(red + green + blue);
            result.worst = max(result.worst, max(red, max(green, blue)));
        }
    }
    result.average = static_cast<double>(sum) / (3.0 * a.width() * a.height());
    return result;
}

}

TEST_CASE(png_from_another_encoder)
{
    auto reference = load("reference.png"_sv);
    EXPECT_EQ(reference->width(), 96);
    EXPECT_EQ(reference->height(), 64);
    // Top-left and bottom-right are the gradient's two sides.
    EXPECT(reference->color_at(0, 0).blue > reference->color_at(0, 63).blue);

    // Adam7 interlacing gives the very same pixels.
    auto interlaced = load("interlaced.png"_sv);
    EXPECT_EQ(compare(*reference, *interlaced).worst, 0);

    // Alpha: half transparent, premultiplied on the way in.
    auto translucent = load("alpha.png"_sv);
    auto pixel = translucent->pixel_at(50, 30);
    EXPECT(pixel_alpha(pixel) >= 127 && pixel_alpha(pixel) <= 128);
    EXPECT(pixel_red(pixel) <= pixel_alpha(pixel));
    auto restored = translucent->color_at(50, 30);
    auto original = reference->color_at(50, 30);
    EXPECT(restored.red >= original.red - 2 && restored.red <= original.red + 2);

    // A palette of 16: what ImageMagick itself reads from that file.
    auto palette = load("palette.png"_sv);
    EXPECT_EQ(compare(*load("palette-as-rgb.png"_sv), *palette).worst, 0);

    // 16-bit grey.
    auto grey = load("grey16.png"_sv);
    auto shade = grey->color_at(20, 20);
    EXPECT(shade.red == shade.green && shade.green == shade.blue);
    EXPECT(compare(*load("grey16-as-rgb.png"_sv), *grey).worst <= 1);
}

TEST_CASE(png_round_trip)
{
    auto reference = load("reference.png"_sv);
    auto encoded = MUST(Image::encode_png(*reference));
    auto decoded = MUST(Image::decode_png(encoded.span()));
    EXPECT_EQ(compare(*reference, *decoded).worst, 0);
    EXPECT(encoded.size() < 96 * 64 * 3);

    auto clear = MUST(Bitmap::create({ 8, 8 }));
    clear->set_pixel_at(1, 1, Color::from_rgb(0xff0000, 128).to_pixel());
    auto clear_encoded = MUST(Image::encode_png(*clear));
    auto clear_decoded = MUST(Image::decode_png(clear_encoded.span()));
    EXPECT_EQ(clear_decoded->pixel_at(0, 0), 0u);
    EXPECT_EQ(clear_decoded->pixel_at(1, 1), clear->pixel_at(1, 1));

    // Empty and tiny.
    auto one = MUST(Bitmap::create({ 1, 1 }));
    one->clear(Color::from_rgb(0x123456));
    auto one_encoded = MUST(Image::encode_png(*one));
    EXPECT_EQ(MUST(Image::decode_png(one_encoded.span()))->pixel_at(0, 0), one->pixel_at(0, 0));
}

namespace {

// A PNG of the given kind, from raw scanlines, built by hand.
Vector<u8> make_png(u32 width, u32 height, u8 depth, u8 color_type, Vector<u8> const& rows, Vector<u8> const* palette = nullptr, Vector<u8> const* transparency = nullptr)
{
    Vector<u8> output;
    u8 const signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    for (u8 byte : signature)
        output.append(byte);
    auto chunk = [&](char const* name, ReadonlyBytes content) {
        auto append_u32 = [&](u32 value) {
            output.append(static_cast<u8>(value >> 24));
            output.append(static_cast<u8>(value >> 16));
            output.append(static_cast<u8>(value >> 8));
            output.append(static_cast<u8>(value));
        };
        append_u32(static_cast<u32>(content.size()));
        size_t start = output.size();
        for (int i = 0; i < 4; ++i)
            output.append(static_cast<u8>(name[i]));
        for (size_t i = 0; i < content.size(); ++i)
            output.append(content[i]);
        append_u32(crc32({ output.data() + start, output.size() - start }));
    };
    u8 header[13] = { static_cast<u8>(width >> 24), static_cast<u8>(width >> 16), static_cast<u8>(width >> 8), static_cast<u8>(width), static_cast<u8>(height >> 24), static_cast<u8>(height >> 16), static_cast<u8>(height >> 8), static_cast<u8>(height), depth, color_type, 0, 0, 0 };
    chunk("IHDR", { header, 13 });
    if (palette)
        chunk("PLTE", palette->span());
    if (transparency)
        chunk("tRNS", transparency->span());
    auto compressed = MUST(Compress::zlib_compress(rows.span()));
    chunk("IDAT", compressed.span());
    chunk("IEND", {});
    return output;
}

}

TEST_CASE(png_kinds)
{
    // 1-bit grey: 0 black, 1 white, packed from the top bit.
    {
        Vector<u8> rows;
        rows.append(0);
        rows.append(0b10100000);
        auto image = MUST(Image::decode_png(make_png(3, 1, 1, 0, rows).span()));
        EXPECT_EQ(image->color_at(0, 0).red, 255);
        EXPECT_EQ(image->color_at(1, 0).red, 0);
        EXPECT_EQ(image->color_at(2, 0).red, 255);
    }
    // 2- and 4-bit grey scale up to 255.
    {
        Vector<u8> rows;
        rows.append(0);
        rows.append(0b00011011);
        auto image = MUST(Image::decode_png(make_png(4, 1, 2, 0, rows).span()));
        EXPECT_EQ(image->color_at(0, 0).red, 0);
        EXPECT_EQ(image->color_at(1, 0).red, 85);
        EXPECT_EQ(image->color_at(2, 0).red, 170);
        EXPECT_EQ(image->color_at(3, 0).red, 255);
        Vector<u8> four;
        four.append(0);
        four.append(0x0f);
        auto image4 = MUST(Image::decode_png(make_png(2, 1, 4, 0, four).span()));
        EXPECT_EQ(image4->color_at(0, 0).red, 0);
        EXPECT_EQ(image4->color_at(1, 0).red, 255);
    }
    // A palette with partial transparency.
    {
        Vector<u8> rows, palette, transparency;
        rows.append(0);
        rows.append(0b00010000);
        u8 const colors[6] = { 10, 20, 30, 200, 100, 50 };
        for (u8 byte : colors)
            palette.append(byte);
        transparency.append(128);
        auto image = MUST(Image::decode_png(make_png(2, 1, 2, 3, rows, &palette, &transparency).span()));
        EXPECT_EQ(image->color_at(0, 0).alpha, 128);
        EXPECT(image->color_at(1, 0) == (Color { 200, 100, 50, 255 }));
        // Index 1: the second color, opaque.
        Vector<u8> second;
        second.append(0);
        second.append(0b01010000);
        auto image2 = MUST(Image::decode_png(make_png(2, 1, 2, 3, second, &palette, &transparency).span()));
        EXPECT(image2->color_at(1, 0) == (Color { 200, 100, 50, 255 }));
    }
    // Every filter, on 8-bit RGB: each row's bytes are differences from the
    // left, above or both.
    {
        Vector<u8> rows;
        // None.
        u8 const none[] = { 0, 10, 20, 30, 40, 50, 60 };
        // Sub: the second pixel is the first plus these.
        u8 const sub[] = { 1, 10, 20, 30, 5, 5, 5 };
        // Up: plus the row above.
        u8 const up[] = { 2, 1, 2, 3, 1, 2, 3 };
        // Average of left and above.
        u8 const average[] = { 3, 2, 2, 2, 3, 3, 3 };
        // Paeth.
        u8 const paeth_row[] = { 4, 1, 1, 1, 1, 1, 1 };
        for (u8 byte : none)
            rows.append(byte);
        for (u8 byte : sub)
            rows.append(byte);
        for (u8 byte : up)
            rows.append(byte);
        for (u8 byte : average)
            rows.append(byte);
        for (u8 byte : paeth_row)
            rows.append(byte);
        auto image = MUST(Image::decode_png(make_png(2, 5, 8, 2, rows).span()));
        EXPECT(image->color_at(0, 0) == (Color { 10, 20, 30, 255 }));
        EXPECT(image->color_at(1, 0) == (Color { 40, 50, 60, 255 }));
        EXPECT(image->color_at(0, 1) == (Color { 10, 20, 30, 255 }));
        EXPECT(image->color_at(1, 1) == (Color { 15, 25, 35, 255 }));
        EXPECT(image->color_at(0, 2) == (Color { 11, 22, 33, 255 }));
        EXPECT(image->color_at(1, 2) == (Color { 16, 27, 38, 255 }));
        // Average: (0 + 11) / 2 + 2 = 7; (22 / 2) + 2 = 13; (33 / 2) + 2 = 18.
        EXPECT(image->color_at(0, 3) == (Color { 7, 13, 18, 255 }));
        // Second pixel: left 7, above 16: 11 + 3 = 14, and so on.
        EXPECT(image->color_at(1, 3) == (Color { 14, 23, 31, 255 }));
        // Paeth: above is the nearest prediction for all of these.
        EXPECT(image->color_at(0, 4) == (Color { 8, 14, 19, 255 }));
        EXPECT(image->color_at(1, 4) == (Color { 15, 24, 32, 255 }));
    }
    // 16-bit RGBA, narrowed to 8 bits; a color key on 8-bit RGB.
    {
        Vector<u8> rows;
        rows.append(0);
        u8 const wide[] = { 0xff, 0xff, 0x80, 0x00, 0x00, 0x00, 0xff, 0xff };
        for (u8 byte : wide)
            rows.append(byte);
        auto image = MUST(Image::decode_png(make_png(1, 1, 16, 6, rows).span()));
        EXPECT(image->color_at(0, 0).red == 255 && image->color_at(0, 0).blue == 0 && image->color_at(0, 0).alpha == 255);
        EXPECT(image->color_at(0, 0).green >= 127 && image->color_at(0, 0).green <= 129);

        Vector<u8> keyed, key;
        keyed.append(0);
        u8 const pixels[] = { 1, 2, 3, 4, 5, 6 };
        for (u8 byte : pixels)
            keyed.append(byte);
        u8 const key_bytes[] = { 0, 1, 0, 2, 0, 3 };
        for (u8 byte : key_bytes)
            key.append(byte);
        auto image2 = MUST(Image::decode_png(make_png(2, 1, 8, 2, keyed, nullptr, &key).span()));
        EXPECT_EQ(image2->color_at(0, 0).alpha, 0);
        EXPECT_EQ(image2->color_at(1, 0).alpha, 255);
    }
}

TEST_CASE(png_refuses_what_is_wrong)
{
    Vector<u8> rows;
    rows.append(0);
    rows.append(0xff);
    auto good = make_png(8, 1, 1, 0, rows);
    EXPECT(!Image::decode_png(good.span()).is_error());

    auto bad_crc = good;
    bad_crc[20] ^= 1;
    EXPECT(Image::decode_png(bad_crc.span()).is_error());
    // A header no format allows: 3-bit grey; zero size.
    EXPECT(Image::decode_png(make_png(8, 1, 3, 0, rows).span()).is_error());
    EXPECT(Image::decode_png(make_png(0, 1, 1, 0, rows).span()).is_error());
    // More rows promised than delivered; an unknown filter.
    EXPECT(Image::decode_png(make_png(8, 9, 1, 0, rows).span()).is_error());
    Vector<u8> bad_filter;
    bad_filter.append(9);
    bad_filter.append(0);
    EXPECT(Image::decode_png(make_png(8, 1, 1, 0, bad_filter).span()).is_error());
    // Not a PNG, or cut short anywhere: an error, never a crash.
    constexpr u8 junk[] = { 1, 2, 3 };
    EXPECT(Image::decode_png({ junk, 3 }).is_error());
    EXPECT(Image::decode({ junk, 3 }).is_error());
    auto file = read_data_file("reference.png"_sv);
    for (size_t length = 0; length < file.size(); length += 97)
        (void)Image::decode_png({ file.data(), length });
    // Noise in the middle: likewise.
    u32 state = 1;
    for (int round = 0; round < 200; ++round) {
        auto copy = file;
        state = state * 1103515245 + 12345;
        copy[8 + (state >> 8) % (copy.size() - 8)] ^= static_cast<u8>(1 + (state >> 24) % 255);
        (void)Image::decode_png(copy.span());
    }
}

TEST_CASE(jpeg_from_other_encoders)
{
    auto reference = load("reference.png"_sv);
    struct Case {
        StringView name;
        double most_average;
        int most_worst;
    };
    Case const cases[] = {
        { "baseline444.jpg"_sv, 2.0, 12 },
        { "baseline420.jpg"_sv, 3.0, 24 },
        { "baseline422.jpg"_sv, 3.0, 24 },
        { "progressive444.jpg"_sv, 2.0, 12 },
        { "progressive420.jpg"_sv, 3.0, 24 },
        { "restart.jpg"_sv, 2.0, 12 },
    };
    for (auto const& one : cases) {
        auto image = load(one.name);
        EXPECT_EQ(image->width(), 96);
        EXPECT_EQ(image->height(), 64);
        auto difference = compare(*reference, *image);
        if (difference.average > one.most_average || difference.worst > one.most_worst) {
            FixedStringBuilder<128> message;
            format_to(message, "{}: average {} worst {}\n", one.name, static_cast<int>(difference.average * 100), difference.worst);
            Test::print(message.view());
        }
        EXPECT(difference.average <= one.most_average);
        EXPECT(difference.worst <= one.most_worst);
    }
    // The two ways of coding the same image give the same result.
    EXPECT(compare(*load("baseline444.jpg"_sv), *load("progressive444.jpg"_sv)).worst <= 2);
    EXPECT(compare(*load("baseline420.jpg"_sv), *load("progressive420.jpg"_sv)).worst <= 2);
    auto grey = load("grey.jpg"_sv);
    auto shade = grey->color_at(48, 32);
    EXPECT(shade.red == shade.green && shade.green == shade.blue);
    EXPECT(shade.alpha == 255);
}

TEST_CASE(jpeg_refuses_what_is_wrong)
{
    auto file = read_data_file("progressive420.jpg"_sv);
    constexpr u8 only_start[] = { 0xff, 0xd8, 0xff };
    EXPECT(Image::decode_jpeg({ only_start, 3 }).is_error());
    for (size_t length = 0; length < file.size(); length += 53)
        (void)Image::decode_jpeg({ file.data(), length });
    u32 state = 7;
    for (int round = 0; round < 300; ++round) {
        auto copy = file;
        state = state * 1103515245 + 12345;
        copy[2 + (state >> 8) % (copy.size() - 2)] ^= static_cast<u8>(1 + (state >> 24) % 255);
        (void)Image::decode_jpeg(copy.span());
    }
}

TEST_CASE(icon_files)
{
    auto small = MUST(Bitmap::create({ 16, 16 }));
    small->clear(Color::from_rgb(0xff0000));
    auto medium = MUST(Bitmap::create({ 32, 32 }));
    medium->clear(Color::from_rgb(0x00ff00));
    auto large = MUST(Bitmap::create({ 64, 64 }));
    large->clear(Color::from_rgb(0x0000ff));
    auto small_png = MUST(Image::encode_png(*small));
    auto medium_png = MUST(Image::encode_png(*medium));
    auto large_png = MUST(Image::encode_png(*large));
    Vector<Image::IconFile::Entry> entries;
    entries.append(Image::IconFile::Entry { 16, 1, small_png.span() });
    entries.append(Image::IconFile::Entry { 16, 2, medium_png.span() });
    entries.append(Image::IconFile::Entry { 32, 2, large_png.span() });
    auto encoded = MUST(Image::encode_icon_file(entries));
    auto file = MUST(Image::IconFile::parse(encoded.span()));
    EXPECT_EQ(file.entries().size(), 3u);
    EXPECT_EQ(MUST(file.bitmap_for(16, 1))->width(), 16);
    EXPECT_EQ(MUST(file.bitmap_for(16, 2))->width(), 32);
    EXPECT_EQ(MUST(file.bitmap_for(32, 2))->width(), 64);
    // Between sizes: the next bigger; beyond them all: the biggest.
    EXPECT_EQ(MUST(file.bitmap_for(20, 1))->width(), 32);
    EXPECT_EQ(MUST(file.bitmap_for(512, 2))->width(), 64);
    // Damaged files are refused.
    constexpr u8 junk[] = { 'V', 'I', 'C', 'N', 2, 0, 0, 0, 0, 0, 0, 0 };
    EXPECT(Image::IconFile::parse({ junk, sizeof(junk) }).is_error());
    EXPECT(Image::IconFile::parse({ encoded.data(), 20 }).is_error());
    auto truncated = encoded;
    truncated[20] = 0xff;
    truncated[21] = 0xff;
    truncated[22] = 0xff;
    EXPECT(Image::IconFile::parse(truncated.span()).is_error());
}
