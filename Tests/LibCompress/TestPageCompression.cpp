/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCompress/PageCompression.h>
#include <LibTest/TestCase.h>

using namespace Compress;

namespace {

constexpr size_t page_length = compressible_page_size;
PageCompressionWorkspace s_workspace;

// Not good randomness: repeatable, and nothing here compresses it.
u32 s_seed = 0x2545f491;
u32 next_random()
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}

// Round trip: returns the compressed size, or 0 if it wouldn't compress into `capacity`.
size_t round_trip(u8 const* page, size_t capacity = page_length)
{
    static u8 compressed[page_length * 2];
    static u8 restored[page_length];
    size_t const size = compress_page(page, { compressed, capacity }, s_workspace);
    if (size == 0)
        return 0;
    EXPECT(size <= capacity);
    __builtin_memset(restored, 0xee, page_length);
    EXPECT(decompress_page({ compressed, size }, restored));
    EXPECT(__builtin_memcmp(restored, page, page_length) == 0);
    return size;
}

}

TEST_CASE(a_page_of_one_value_is_small)
{
    static u8 page[page_length];
    __builtin_memset(page, 0, page_length);
    EXPECT(round_trip(page) < 130);
    __builtin_memset(page, 0xa5, page_length);
    EXPECT(round_trip(page) < 130);
}

TEST_CASE(what_programs_keep_in_memory_compresses)
{
    // Pointers into one area, and small integers: much of what a heap has in it.
    static u64 page[page_length / sizeof(u64)];
    for (size_t i = 0; i < page_length / sizeof(u64); ++i)
        page[i] = i % 3 == 0 ? 0x0000'7e12'3456'0000ull + (next_random() % 64) * 16 : next_random() % 256;
    size_t const size = round_trip(reinterpret_cast<u8 const*>(page));
    EXPECT(size > 0);
    EXPECT(size < page_length * 3 / 4);
}

TEST_CASE(repeats_near_and_far)
{
    static u8 page[page_length];
    for (size_t i = 0; i < page_length; ++i)
        page[i] = static_cast<u8>(next_random());
    // The same stretch again a long way on, and a short pattern close by.
    __builtin_memcpy(page + 3000, page + 10, 1000);
    for (size_t i = 1000; i < 1200; ++i)
        page[i] = page[i - 7];
    size_t const size = round_trip(page);
    EXPECT(size > 0);
    EXPECT(size < page_length - 1000);
}

TEST_CASE(every_length_of_every_kind_of_copy)
{
    static u8 page[page_length];
    for (size_t length = 1; length < 300; ++length) {
        for (size_t i = 0; i < page_length; ++i)
            page[i] = static_cast<u8>(next_random());
        // Near, then far.
        __builtin_memmove(page + 500, page + 400, length);
        __builtin_memcpy(page + 2000, page + 100, length);
        EXPECT(round_trip(page, page_length * 2) > 0);
    }
}

TEST_CASE(a_copy_that_reaches_the_very_end)
{
    static u8 page[page_length];
    for (size_t i = 0; i < page_length; ++i)
        page[i] = static_cast<u8>(next_random());
    __builtin_memset(page + page_length - 300, 7, 300);
    EXPECT(round_trip(page, page_length * 2) > 0);
    // And what is one byte short of a copy.
    for (size_t tail = 1; tail < 8; ++tail) {
        __builtin_memcpy(page + page_length - tail, page, tail);
        EXPECT(round_trip(page, page_length * 2) > 0);
    }
}

TEST_CASE(what_does_not_compress_says_so)
{
    static u8 page[page_length];
    for (size_t i = 0; i < page_length; ++i)
        page[i] = static_cast<u8>(next_random() >> 7);
    EXPECT_EQ(round_trip(page, 2024), 0u);
    // And given room, it still comes back.
    EXPECT(round_trip(page, page_length * 2) > 0);
}

TEST_CASE(never_writes_past_what_it_was_given)
{
    static u8 page[page_length];
    static u8 output[page_length + 64];
    for (size_t i = 0; i < page_length; ++i)
        page[i] = (i % 97 < 60) ? static_cast<u8>(next_random()) : 0;
    for (size_t capacity = 0; capacity < 3000; capacity += 7) {
        __builtin_memset(output, 0x5c, sizeof(output));
        size_t const size = compress_page(page, { output, capacity }, s_workspace);
        EXPECT(size <= capacity);
        for (size_t i = capacity; i < sizeof(output); ++i) {
            EXPECT(output[i] == 0x5c);
            if (output[i] != 0x5c)
                return;
        }
    }
}

TEST_CASE(a_stream_that_is_cut_short_is_refused)
{
    static u8 page[page_length];
    static u8 compressed[page_length];
    static u8 restored[page_length];
    for (size_t i = 0; i < page_length; ++i)
        page[i] = static_cast<u8>(i % 251 < 100 ? next_random() : 3);
    size_t const size = compress_page(page, { compressed, sizeof(compressed) }, s_workspace);
    EXPECT(size > 0);
    for (size_t cut = 0; cut < size; ++cut)
        EXPECT(!decompress_page({ compressed, cut }, restored));
}

TEST_CASE(streams_that_lie_are_refused)
{
    static u8 restored[page_length];
    // Copying from before the start.
    u8 const before_start[] = { 0x00, 'a', 0x40, 0x05 };
    EXPECT(!decompress_page({ before_start, sizeof(before_start) }, restored));
    // From nothing at all.
    u8 const nothing[] = { 0x80, 0x00, 0x00 };
    EXPECT(!decompress_page({ nothing, sizeof(nothing) }, restored));
    // Fewer than a page.
    u8 const too_short[] = { 0x00, 'a' };
    EXPECT(!decompress_page({ too_short, sizeof(too_short) }, restored));
    // More than a page: 64 of them.
    static u8 too_long[66 * 2 * 64];
    size_t at = 0;
    too_long[at++] = 0x00;
    too_long[at++] = 'a';
    while (at + 2 <= sizeof(too_long)) {
        too_long[at++] = 0x7f;
        too_long[at++] = 0x00;
    }
    EXPECT(!decompress_page({ too_long, at }, restored));
}

TEST_CASE(damage_does_not_reach_outside_the_page)
{
    static u8 page[page_length];
    static u8 compressed[page_length];
    static u8 damaged[page_length];
    // The page is in the middle, so that writing outside it shows.
    static u8 surroundings[page_length * 3];
    for (size_t i = 0; i < page_length; ++i)
        page[i] = static_cast<u8>(i % 61 < 20 ? next_random() : i / 64);
    size_t const size = compress_page(page, { compressed, sizeof(compressed) }, s_workspace);
    EXPECT(size > 0);
    for (int round = 0; round < 4000; ++round) {
        __builtin_memcpy(damaged, compressed, size);
        for (int flips = 0; flips < 1 + round % 8; ++flips)
            damaged[next_random() % size] ^= static_cast<u8>(1u << (next_random() % 8));
        __builtin_memset(surroundings, 0x33, sizeof(surroundings));
        (void)decompress_page({ damaged, size }, surroundings + page_length);
        for (size_t i = 0; i < page_length; ++i) {
            bool const untouched = surroundings[i] == 0x33 && surroundings[page_length * 2 + i] == 0x33;
            EXPECT(untouched);
            if (!untouched)
                return;
        }
    }
}

TEST_CASE(many_random_pages)
{
    static u8 page[page_length];
    for (int round = 0; round < 2000; ++round) {
        // Stretches of everything from noise to one value, of all lengths.
        size_t at = 0;
        while (at < page_length) {
            size_t const stretch = min<size_t>(page_length - at, 1 + next_random() % 700);
            switch (next_random() % 4) {
            case 0:
                for (size_t i = 0; i < stretch; ++i)
                    page[at + i] = static_cast<u8>(next_random());
                break;
            case 1:
                __builtin_memset(page + at, static_cast<int>(next_random() & 0xff), stretch);
                break;
            case 2:
                if (at > 0) {
                    size_t const back = 1 + next_random() % at;
                    for (size_t i = 0; i < stretch; ++i)
                        page[at + i] = page[at + i - back];
                    break;
                }
                [[fallthrough]];
            default: {
                u32 const word = next_random();
                for (size_t i = 0; i < stretch; ++i)
                    page[at + i] = reinterpret_cast<u8 const*>(&word)[i % 4];
                break;
            }
            }
            at += stretch;
        }
        (void)round_trip(page, page_length * 2);
    }
}
