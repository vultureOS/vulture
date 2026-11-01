/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Checksum.h>
#include <LibCompress/Deflate.h>
#include <LibGfx/Color.h>
#include <LibImage/Image.h>

namespace Image {

namespace {

constexpr u8 signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

constexpr u32 tag(char a, char b, char c, char d)
{
    return (static_cast<u32>(static_cast<u8>(a)) << 24) | (static_cast<u32>(static_cast<u8>(b)) << 16) | (static_cast<u32>(static_cast<u8>(c)) << 8) | static_cast<u8>(d);
}

u32 read_u32(u8 const* bytes)
{
    return (static_cast<u32>(bytes[0]) << 24) | (static_cast<u32>(bytes[1]) << 16) | (static_cast<u32>(bytes[2]) << 8) | bytes[3];
}

u32 png_crc(u8 const* data, size_t size)
{
    return crc32({ data, size });
}

u8 paeth(int left, int above, int upper_left)
{
    int estimate = left + above - upper_left;
    int to_left = estimate > left ? estimate - left : left - estimate;
    int to_above = estimate > above ? estimate - above : above - estimate;
    int to_upper_left = estimate > upper_left ? estimate - upper_left : upper_left - estimate;
    if (to_left <= to_above && to_left <= to_upper_left)
        return static_cast<u8>(left);
    return static_cast<u8>(to_above <= to_upper_left ? above : upper_left);
}

struct Header {
    u32 width { 0 };
    u32 height { 0 };
    u8 bit_depth { 0 };
    u8 color_type { 0 };
    u8 interlace { 0 };

    [[nodiscard]] u32 channels() const
    {
        switch (color_type) {
        case 0:
        case 3:
            return 1;
        case 2:
            return 3;
        case 4:
            return 2;
        default:
            return 4;
        }
    }
    [[nodiscard]] u32 bits_per_pixel() const { return channels() * bit_depth; }
    // What each filter treats as "one pixel back", in whole bytes.
    [[nodiscard]] u32 filter_step() const { return max(1u, bits_per_pixel() / 8); }
    [[nodiscard]] size_t row_bytes(u32 columns) const { return (static_cast<size_t>(columns) * bits_per_pixel() + 7) / 8; }
};

bool is_valid(Header const& header)
{
    if (header.width == 0 || header.height == 0 || header.width > 32768 || header.height > 32768 || header.interlace > 1)
        return false;
    u8 depth = header.bit_depth;
    switch (header.color_type) {
    case 0:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 2:
    case 4:
    case 6:
        return depth == 8 || depth == 16;
    case 3:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    default:
        return false;
    }
}

// One reduced image (or the whole one) out of the decompressed data: its rows
// unfiltered in place, and each pixel handed to `store`.
struct Decoder {
    Header const& header;
    Span<u8> data;
    size_t position { 0 };
    Vector<u8> const& palette;
    Vector<u8> const& transparency;
    Gfx::Bitmap& bitmap;

    ErrorOr<void> read_pass(u32 columns, u32 rows, u32 start_x, u32 start_y, u32 step_x, u32 step_y)
    {
        if (columns == 0 || rows == 0)
            return {};
        size_t row_size = header.row_bytes(columns);
        u32 step = header.filter_step();
        u8 const* previous = nullptr;
        for (u32 y = 0; y < rows; ++y) {
            if (position + 1 + row_size > data.size())
                return Error::from_string_literal("PNG: not enough image data");
            u8 filter = data[position];
            u8* row = data.data() + position + 1;
            position += 1 + row_size;
            for (size_t i = 0; i < row_size; ++i) {
                int left = i >= step ? row[i - step] : 0;
                int above = previous ? previous[i] : 0;
                int upper_left = (previous && i >= step) ? previous[i - step] : 0;
                switch (filter) {
                case 0:
                    break;
                case 1:
                    row[i] = static_cast<u8>(row[i] + left);
                    break;
                case 2:
                    row[i] = static_cast<u8>(row[i] + above);
                    break;
                case 3:
                    row[i] = static_cast<u8>(row[i] + (left + above) / 2);
                    break;
                case 4:
                    row[i] = static_cast<u8>(row[i] + paeth(left, above, upper_left));
                    break;
                default:
                    return Error::from_string_literal("PNG: unknown filter");
                }
            }
            auto* output = bitmap.scanline(static_cast<int>(start_y + y * step_y));
            for (u32 x = 0; x < columns; ++x)
                output[start_x + x * step_x] = pixel_at(row, x).to_pixel();
            previous = row;
        }
        return {};
    }

    [[nodiscard]] Gfx::Color pixel_at(u8 const* row, u32 x) const
    {
        u32 depth = header.bit_depth;
        auto sample16 = [&](u32 index) { return (static_cast<u32>(row[index * 2]) << 8) | row[index * 2 + 1]; };
        auto narrow = [](u32 value) { return static_cast<u8>((value * 255 + 32767) / 65535); };
        switch (header.color_type) {
        case 0: {
            u32 value;
            u32 opaque_key = transparency.size() >= 2 ? (static_cast<u32>(transparency[0]) << 8) | transparency[1] : 0x10000;
            if (depth == 16) {
                value = sample16(x);
                u8 grey = narrow(value);
                return { grey, grey, grey, static_cast<u8>(value == opaque_key ? 0 : 255) };
            }
            if (depth == 8) {
                value = row[x];
            } else {
                u32 bit_index = x * depth;
                value = (row[bit_index / 8] >> (8 - depth - bit_index % 8)) & ((1u << depth) - 1);
            }
            auto grey = static_cast<u8>(value * 255 / ((1u << depth) - 1));
            return { grey, grey, grey, static_cast<u8>(value == opaque_key ? 0 : 255) };
        }
        case 2: {
            if (depth == 16) {
                u32 r = sample16(x * 3), g = sample16(x * 3 + 1), b = sample16(x * 3 + 2);
                bool keyed = transparency.size() >= 6 && r == ((static_cast<u32>(transparency[0]) << 8) | transparency[1]) && g == ((static_cast<u32>(transparency[2]) << 8) | transparency[3]) && b == ((static_cast<u32>(transparency[4]) << 8) | transparency[5]);
                return { narrow(r), narrow(g), narrow(b), static_cast<u8>(keyed ? 0 : 255) };
            }
            u8 r = row[x * 3], g = row[x * 3 + 1], b = row[x * 3 + 2];
            bool keyed = transparency.size() >= 6 && r == transparency[1] && g == transparency[3] && b == transparency[5];
            return { r, g, b, static_cast<u8>(keyed ? 0 : 255) };
        }
        case 3: {
            u32 bit_index = x * depth;
            u32 index = depth == 8 ? row[x] : ((row[bit_index / 8] >> (8 - depth - bit_index % 8)) & ((1u << depth) - 1));
            if (index * 3 + 2 >= palette.size())
                return { 0, 0, 0, 255 };
            u8 alpha = index < transparency.size() ? transparency[index] : 255;
            return { palette[index * 3], palette[index * 3 + 1], palette[index * 3 + 2], alpha };
        }
        case 4:
            if (depth == 16) {
                u8 grey = narrow(sample16(x * 2));
                return { grey, grey, grey, narrow(sample16(x * 2 + 1)) };
            }
            return { row[x * 2], row[x * 2], row[x * 2], row[x * 2 + 1] };
        default:
            if (depth == 16)
                return { narrow(sample16(x * 4)), narrow(sample16(x * 4 + 1)), narrow(sample16(x * 4 + 2)), narrow(sample16(x * 4 + 3)) };
            return { row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3] };
        }
    }
};

}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> decode_png(ReadonlyBytes bytes)
{
    if (bytes.size() < 8 || __builtin_memcmp(bytes.data(), signature, 8) != 0)
        return Error::from_string_literal("PNG: not a PNG file");
    Header header;
    Vector<u8> palette;
    Vector<u8> transparency;
    Vector<u8> compressed;
    bool seen_end = false;
    size_t position = 8;
    while (position + 12 <= bytes.size() && !seen_end) {
        u32 length = read_u32(bytes.data() + position);
        if (length > bytes.size() - position - 12)
            return Error::from_string_literal("PNG: a chunk runs past the end of the file");
        u32 type = read_u32(bytes.data() + position + 4);
        u8 const* content = bytes.data() + position + 8;
        u32 stored_crc = read_u32(content + length);
        if (png_crc(bytes.data() + position + 4, length + 4) != stored_crc)
            return Error::from_string_literal("PNG: a chunk's checksum is wrong");
        switch (type) {
        case tag('I', 'H', 'D', 'R'):
            if (length != 13)
                return Error::from_string_literal("PNG: bad header");
            header.width = read_u32(content);
            header.height = read_u32(content + 4);
            header.bit_depth = content[8];
            header.color_type = content[9];
            header.interlace = content[12];
            if (content[10] != 0 || content[11] != 0 || !is_valid(header))
                return Error::from_string_literal("PNG: unsupported header");
            break;
        case tag('P', 'L', 'T', 'E'):
            TRY(palette.try_resize(length));
            __builtin_memcpy(palette.data(), content, length);
            break;
        case tag('t', 'R', 'N', 'S'):
            TRY(transparency.try_resize(length));
            __builtin_memcpy(transparency.data(), content, length);
            break;
        case tag('I', 'D', 'A', 'T'): {
            size_t offset = compressed.size();
            TRY(compressed.try_resize(offset + length));
            __builtin_memcpy(compressed.data() + offset, content, length);
            break;
        }
        case tag('I', 'E', 'N', 'D'):
            seen_end = true;
            break;
        default:
            break;
        }
        position += 12 + length;
    }
    if (header.width == 0 || compressed.is_empty())
        return Error::from_string_literal("PNG: no image in the file");
    if (header.color_type == 3 && palette.is_empty())
        return Error::from_string_literal("PNG: no palette");

    // The whole image's data, room for each pass's filter bytes included.
    size_t limit = (header.row_bytes(header.width) + 1) * header.height;
    if (header.interlace)
        limit = limit * 2 + 1024;
    auto raw = TRY(Compress::zlib_decompress(compressed.span(), limit));
    auto bitmap = TRY(Gfx::Bitmap::create({ static_cast<int>(header.width), static_cast<int>(header.height) }));
    Decoder decoder { header, raw.span(), 0, palette, transparency, *bitmap };
    if (header.interlace == 0) {
        TRY(decoder.read_pass(header.width, header.height, 0, 0, 1, 1));
        return bitmap;
    }
    // Adam7: seven reduced images, each a regular grid of the pixels.
    struct Pass {
        u32 start_x, start_y, step_x, step_y;
    };
    static constexpr Pass passes[7] = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };
    for (auto const& pass : passes) {
        u32 columns = header.width > pass.start_x ? (header.width - pass.start_x + pass.step_x - 1) / pass.step_x : 0;
        u32 rows = header.height > pass.start_y ? (header.height - pass.start_y + pass.step_y - 1) / pass.step_y : 0;
        TRY(decoder.read_pass(columns, rows, pass.start_x, pass.start_y, pass.step_x, pass.step_y));
    }
    return bitmap;
}

namespace {

ErrorOr<void> append_chunk(Vector<u8>& output, u32 type, ReadonlyBytes content)
{
    auto append_u32 = [&](u32 value) -> ErrorOr<void> {
        TRY(output.try_append(static_cast<u8>(value >> 24)));
        TRY(output.try_append(static_cast<u8>(value >> 16)));
        TRY(output.try_append(static_cast<u8>(value >> 8)));
        TRY(output.try_append(static_cast<u8>(value)));
        return {};
    };
    TRY(append_u32(static_cast<u32>(content.size())));
    size_t start = output.size();
    TRY(append_u32(type));
    size_t body = output.size();
    TRY(output.try_resize(body + content.size()));
    if (content.size())
        __builtin_memcpy(output.data() + body, content.data(), content.size());
    u32 crc = png_crc(output.data() + start, output.size() - start);
    TRY(append_u32(crc));
    return {};
}

}

ErrorOr<Vector<u8>> encode_png(Gfx::Bitmap const& bitmap)
{
    int width = bitmap.width();
    int height = bitmap.height();
    bool opaque = true;
    for (int y = 0; y < height && opaque; ++y) {
        auto const* row = bitmap.scanline(y);
        for (int x = 0; x < width && opaque; ++x)
            opaque = Gfx::pixel_alpha(row[x]) == 255;
    }
    size_t channels = opaque ? 3 : 4;
    size_t row_size = static_cast<size_t>(width) * channels;
    Vector<u8> filtered;
    TRY(filtered.try_resize((row_size + 1) * static_cast<size_t>(height)));
    Vector<u8> current_row, previous_row, candidate, best;
    TRY(current_row.try_resize(row_size));
    TRY(previous_row.try_resize(row_size));
    TRY(candidate.try_resize(row_size));
    TRY(best.try_resize(row_size));
    if (row_size)
        __builtin_memset(previous_row.data(), 0, row_size);
    size_t output_position = 0;
    for (int y = 0; y < height; ++y) {
        auto const* source = bitmap.scanline(y);
        for (int x = 0; x < width; ++x) {
            auto color = Gfx::Color::from_pixel(source[x]);
            u8* out = current_row.data() + static_cast<size_t>(x) * channels;
            out[0] = color.red;
            out[1] = color.green;
            out[2] = color.blue;
            if (channels == 4)
                out[3] = color.alpha;
        }
        // The filter that leaves the smallest values: the usual heuristic.
        u64 best_score = ~0ull;
        u8 best_filter = 0;
        for (u8 filter = 0; filter < 5; ++filter) {
            u64 score = 0;
            for (size_t i = 0; i < row_size; ++i) {
                int left = i >= channels ? current_row[i - channels] : 0;
                int above = previous_row[i];
                int upper_left = i >= channels ? previous_row[i - channels] : 0;
                int predicted = 0;
                switch (filter) {
                case 1:
                    predicted = left;
                    break;
                case 2:
                    predicted = above;
                    break;
                case 3:
                    predicted = (left + above) / 2;
                    break;
                case 4:
                    predicted = paeth(left, above, upper_left);
                    break;
                default:
                    break;
                }
                auto value = static_cast<u8>(current_row[i] - predicted);
                candidate[i] = value;
                score += value < 128 ? value : 256 - value;
            }
            if (score < best_score) {
                best_score = score;
                best_filter = filter;
                swap(best, candidate);
            }
        }
        filtered[output_position++] = best_filter;
        if (row_size)
            __builtin_memcpy(filtered.data() + output_position, best.data(), row_size);
        output_position += row_size;
        swap(current_row, previous_row);
    }
    auto compressed = TRY(Compress::zlib_compress(filtered.span()));

    Vector<u8> output;
    TRY(output.try_resize(8));
    __builtin_memcpy(output.data(), signature, 8);
    u8 ihdr[13] = {
        static_cast<u8>(width >> 24), static_cast<u8>(width >> 16), static_cast<u8>(width >> 8), static_cast<u8>(width),
        static_cast<u8>(height >> 24), static_cast<u8>(height >> 16), static_cast<u8>(height >> 8), static_cast<u8>(height),
        8, static_cast<u8>(opaque ? 2 : 6), 0, 0, 0
    };
    TRY(append_chunk(output, tag('I', 'H', 'D', 'R'), { ihdr, sizeof(ihdr) }));
    TRY(append_chunk(output, tag('I', 'D', 'A', 'T'), compressed.span()));
    TRY(append_chunk(output, tag('I', 'E', 'N', 'D'), {}));
    return output;
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> decode(ReadonlyBytes bytes)
{
    if (bytes.size() >= 8 && __builtin_memcmp(bytes.data(), signature, 8) == 0)
        return decode_png(bytes);
    if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
        return decode_jpeg(bytes);
    return Error::from_string_literal("not an image format we know");
}

namespace {

u32 read_le32(u8 const* bytes)
{
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) | (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
}

}

ErrorOr<IconFile> IconFile::parse(ReadonlyBytes bytes)
{
    if (bytes.size() < 12 || __builtin_memcmp(bytes.data(), "VICN", 4) != 0)
        return Error::from_string_literal("not an icon file");
    if (read_le32(bytes.data() + 4) != 1)
        return Error::from_string_literal("an icon file of an unknown version");
    u32 count = read_le32(bytes.data() + 8);
    if (count > (bytes.size() - 12) / 16)
        return Error::from_string_literal("a truncated icon file");
    IconFile file;
    for (u32 i = 0; i < count; ++i) {
        u8 const* record = bytes.data() + 12 + static_cast<size_t>(i) * 16;
        u32 offset = read_le32(record + 8);
        u32 length = read_le32(record + 12);
        if (offset > bytes.size() || length > bytes.size() - offset)
            return Error::from_string_literal("a truncated icon file");
        TRY(file.m_entries.try_append(Entry { read_le32(record), read_le32(record + 4), { bytes.data() + offset, length } }));
    }
    return file;
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> IconFile::bitmap_for(u32 points, u32 scale) const
{
    if (m_entries.is_empty())
        return Error::from_string_literal("an icon file with no images");
    u64 wanted = static_cast<u64>(points) * scale;
    Entry const* chosen = nullptr;
    auto pixels_of = [](Entry const& entry) { return static_cast<u64>(entry.points) * entry.scale; };
    for (auto const& entry : m_entries) {
        if (!chosen) {
            chosen = &entry;
            continue;
        }
        u64 size = pixels_of(entry), best = pixels_of(*chosen);
        bool better = best < wanted ? size > best : (size >= wanted && size < best);
        // Among those the same size, one drawn for this scale.
        if (better || (size == best && entry.scale == scale))
            chosen = &entry;
    }
    return decode_png(chosen->data);
}

ErrorOr<Vector<u8>> encode_icon_file(Vector<IconFile::Entry> const& entries)
{
    Vector<u8> output;
    auto append_u32 = [&](u32 value) -> ErrorOr<void> {
        for (int shift = 0; shift < 32; shift += 8)
            TRY(output.try_append(static_cast<u8>(value >> shift)));
        return {};
    };
    TRY(output.try_append(static_cast<u8>('V')));
    TRY(output.try_append(static_cast<u8>('I')));
    TRY(output.try_append(static_cast<u8>('C')));
    TRY(output.try_append(static_cast<u8>('N')));
    TRY(append_u32(1));
    TRY(append_u32(static_cast<u32>(entries.size())));
    u32 offset = 12 + static_cast<u32>(entries.size()) * 16;
    for (auto const& entry : entries) {
        TRY(append_u32(entry.points));
        TRY(append_u32(entry.scale));
        TRY(append_u32(offset));
        TRY(append_u32(static_cast<u32>(entry.data.size())));
        offset += static_cast<u32>(entry.data.size());
    }
    for (auto const& entry : entries) {
        size_t at = output.size();
        TRY(output.try_resize(at + entry.data.size()));
        if (entry.data.size())
            __builtin_memcpy(output.data() + at, entry.data.data(), entry.data.size());
    }
    return output;
}

}
