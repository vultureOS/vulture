/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/HashMap.h>
#include <LibForeignFS/Volume.h>

namespace ForeignFS {

// A store, seen as bytes: pieces of it kept in memory until they're flushed, or there are too many.
class SectorCache {
public:
    static constexpr size_t piece_size = 4096;

    SectorCache(Store& store, size_t most_pieces = 2048)
        : m_store(store)
        , m_most_pieces(most_pieces)
    {
    }
    ~SectorCache();

    [[nodiscard]] Store& store() { return m_store; }
    [[nodiscard]] u64 size() const { return m_store.sector_count() * Store::sector_size; }

    ErrorOr<void> read(u64 offset, Bytes);
    ErrorOr<void> write(u64 offset, ReadonlyBytes);
    // As though `count` bytes of zeros had been written.
    ErrorOr<void> zero(u64 offset, u64 count);
    ErrorOr<void> flush();

private:
    struct Piece {
        u8* bytes { nullptr };
        u64 last_used { 0 };
        bool dirty { false };
    };

    ErrorOr<Piece*> piece_at(u64 index, bool will_replace);
    ErrorOr<void> write_out(u64 index, Piece&);
    ErrorOr<void> make_room();

    Store& m_store;
    size_t m_most_pieces;
    u64 m_uses { 0 };
    HashMap<u64, Piece> m_pieces;
};

}
