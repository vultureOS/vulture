/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>

// Cyclic redundancy checks. CRC-32C (Castagnoli) guards vultureFS
// metadata; CRC-32 (IEEE 802.3, as in zlib and GPT) guards partition
// tables. Both are table-driven, eight bytes at a time.

namespace Base {

namespace Detail {

struct CRCTables {
    u32 table[8][256];
};

consteval CRCTables make_crc_tables(u32 reflected_polynomial)
{
    CRCTables tables {};
    for (u32 i = 0; i < 256; ++i) {
        u32 crc = i;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? reflected_polynomial : 0);
        tables.table[0][i] = crc;
    }
    for (u32 i = 0; i < 256; ++i) {
        for (int slice = 1; slice < 8; ++slice)
            tables.table[slice][i] = (tables.table[slice - 1][i] >> 8) ^ tables.table[0][tables.table[slice - 1][i] & 0xff];
    }
    return tables;
}

inline constexpr CRCTables crc32c_tables = make_crc_tables(0x82f6'3b78);
inline constexpr CRCTables crc32_tables = make_crc_tables(0xedb8'8320);

constexpr u32 crc_update(CRCTables const& tables, u32 crc, u8 const* data, size_t size)
{
    while (size >= 8) {
        u32 low = crc ^ (static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) | (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24));
        u32 high = static_cast<u32>(data[4]) | (static_cast<u32>(data[5]) << 8) | (static_cast<u32>(data[6]) << 16) | (static_cast<u32>(data[7]) << 24);
        crc = tables.table[7][low & 0xff] ^ tables.table[6][(low >> 8) & 0xff] ^ tables.table[5][(low >> 16) & 0xff] ^ tables.table[4][low >> 24]
            ^ tables.table[3][high & 0xff] ^ tables.table[2][(high >> 8) & 0xff] ^ tables.table[1][(high >> 16) & 0xff] ^ tables.table[0][high >> 24];
        data += 8;
        size -= 8;
    }
    while (size-- > 0)
        crc = (crc >> 8) ^ tables.table[0][(crc ^ *data++) & 0xff];
    return crc;
}

}

// Feed data in pieces with `update`; `crc32c(bytes)` does it in one go.
class CRC32C {
public:
    constexpr void update(ReadonlyBytes bytes) { m_state = Detail::crc_update(Detail::crc32c_tables, m_state, bytes.data(), bytes.size()); }
    [[nodiscard]] constexpr u32 digest() const { return ~m_state; }

private:
    u32 m_state { ~0u };
};

class CRC32 {
public:
    constexpr void update(ReadonlyBytes bytes) { m_state = Detail::crc_update(Detail::crc32_tables, m_state, bytes.data(), bytes.size()); }
    [[nodiscard]] constexpr u32 digest() const { return ~m_state; }

private:
    u32 m_state { ~0u };
};

constexpr u32 crc32c(ReadonlyBytes bytes)
{
    CRC32C crc;
    crc.update(bytes);
    return crc.digest();
}

constexpr u32 crc32(ReadonlyBytes bytes)
{
    CRC32 crc;
    crc.update(bytes);
    return crc.digest();
}

}

using Base::crc32;
using Base::CRC32;
using Base::crc32c;
using Base::CRC32C;
