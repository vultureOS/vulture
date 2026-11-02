/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCompress/PageCompression.h>

namespace Compress {

namespace {

constexpr size_t literal_limit = 64;
constexpr size_t near_offset_limit = 256;
constexpr size_t near_length_minimum = 3;
constexpr size_t near_length_limit = 66;
constexpr size_t far_length_minimum = 4;
constexpr size_t far_length_limit = 131;
constexpr unsigned table_bits = 10;

u32 load_word(u8 const* at)
{
    u32 word;
    __builtin_memcpy(&word, at, sizeof(word));
    return word;
}

}

size_t compress_page(u8 const* page, Bytes output, PageCompressionWorkspace& workspace)
{
    constexpr size_t page_length = compressible_page_size;
    __builtin_memset(workspace.positions, 0, sizeof(workspace.positions));
    u8* const out = output.data();
    size_t const capacity = output.size();
    size_t written = 0;

    auto write_literals = [&](size_t from, size_t to) {
        while (from < to) {
            size_t const count = min(to - from, literal_limit);
            if (written + 1 + count > capacity)
                return false;
            out[written++] = static_cast<u8>(count - 1);
            __builtin_memcpy(out + written, page + from, count);
            written += count;
            from += count;
        }
        return true;
    };

    size_t position = 0;
    size_t literals_from = 0;
    size_t misses = 0;
    while (position + sizeof(u32) <= page_length) {
        u32 const word = load_word(page + position);
        u32 const slot = (word * 2654435761u) >> (32 - table_bits);
        size_t const candidate = workspace.positions[slot];
        workspace.positions[slot] = static_cast<u16>(position + 1);
        if (candidate == 0 || load_word(page + candidate - 1) != word) {
            // The longer nothing has repeated, the less likely it is to: go through it faster.
            ++misses;
            position += 1 + (misses >> 5);
            continue;
        }
        misses = 0;
        size_t const from = candidate - 1;
        size_t length = sizeof(u32);
        while (position + length < page_length && page[from + length] == page[position + length])
            ++length;
        if (!write_literals(literals_from, position))
            return 0;
        size_t const offset = position - from;
        size_t remaining = length;
        if (offset <= near_offset_limit) {
            while (remaining >= near_length_minimum) {
                size_t const count = min(remaining, near_length_limit);
                if (written + 2 > capacity)
                    return 0;
                out[written++] = static_cast<u8>(0x40 | (count - near_length_minimum));
                out[written++] = static_cast<u8>(offset - 1);
                remaining -= count;
            }
        } else {
            while (remaining >= far_length_minimum) {
                size_t const count = min(remaining, far_length_limit);
                if (written + 3 > capacity)
                    return 0;
                out[written++] = static_cast<u8>(0x80 | (count - far_length_minimum));
                out[written++] = static_cast<u8>((offset - 1) & 0xff);
                out[written++] = static_cast<u8>((offset - 1) >> 8);
                remaining -= count;
            }
        }
        // Too little left over to say as a copy: it goes with the literals after this.
        position += length - remaining;
        literals_from = position;
    }
    if (!write_literals(literals_from, page_length))
        return 0;
    return written;
}

bool decompress_page(ReadonlyBytes input, u8* page)
{
    constexpr size_t page_length = compressible_page_size;
    u8 const* const in = input.data();
    size_t const size = input.size();
    size_t read = 0;
    size_t done = 0;
    while (read < size) {
        u8 const first = in[read++];
        if ((first & 0xc0) == 0) {
            size_t const count = static_cast<size_t>(first) + 1;
            if (count > size - read || count > page_length - done)
                return false;
            __builtin_memcpy(page + done, in + read, count);
            read += count;
            done += count;
            continue;
        }
        size_t count;
        size_t offset;
        if (first & 0x80) {
            if (size - read < 2)
                return false;
            count = static_cast<size_t>(first & 0x7f) + far_length_minimum;
            offset = (static_cast<size_t>(in[read]) | (static_cast<size_t>(in[read + 1]) << 8)) + 1;
            read += 2;
        } else {
            if (size - read < 1)
                return false;
            count = static_cast<size_t>(first & 0x3f) + near_length_minimum;
            offset = static_cast<size_t>(in[read]) + 1;
            read += 1;
        }
        if (offset > done || count > page_length - done)
            return false;
        // A byte at a time, as this may be copying what it has only just written.
        for (size_t i = 0; i < count; ++i)
            page[done + i] = page[done + i - offset];
        done += count;
    }
    return done == page_length;
}

}
