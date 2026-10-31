/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Checksum.h>
#include <LibTest/TestCase.h>

namespace {

ReadonlyBytes text(char const* characters)
{
    return { reinterpret_cast<u8 const*>(characters), __builtin_strlen(characters) };
}

}

TEST_CASE(crc32c_check_values)
{
    // The standard check value: the CRC of "123456789".
    EXPECT_EQ(crc32c(text("123456789")), 0xe306'9283u);
    EXPECT_EQ(crc32c({}), 0u);
    // RFC 3720 (iSCSI) test vectors: 32 bytes of zeros, of ones, ascending.
    u8 zeros[32] {}, ones[32], ascending[32];
    for (u8 i = 0; i < 32; ++i) {
        ones[i] = 0xff;
        ascending[i] = i;
    }
    EXPECT_EQ(crc32c(zeros), 0x8a91'36aau);
    EXPECT_EQ(crc32c(ones), 0x62a8'ab43u);
    EXPECT_EQ(crc32c(ascending), 0x46dd'794eu);
}

TEST_CASE(crc32_check_values)
{
    EXPECT_EQ(crc32(text("123456789")), 0xcbf4'3926u);
    EXPECT_EQ(crc32(text("The quick brown fox jumps over the lazy dog")), 0x414f'a339u);
}

TEST_CASE(crc_in_pieces_matches_one_go)
{
    auto whole = text("vultureFS checksums every metadata block it writes.");
    for (size_t split = 0; split <= whole.size(); ++split) {
        CRC32C crc;
        crc.update(whole.slice(0, split));
        crc.update(whole.slice(split));
        EXPECT_EQ(crc.digest(), crc32c(whole));
    }
    static_assert(crc32c({}) == 0);
}
