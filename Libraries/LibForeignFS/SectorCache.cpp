/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibBase/New.h>
#include <LibBase/QuickSort.h>
#include <LibForeignFS/SectorCache.h>

namespace ForeignFS {

SectorCache::~SectorCache()
{
    m_pieces.for_each([](u64, Piece& piece) { operator delete[](piece.bytes, Base::nothrow); });
}

ErrorOr<void> SectorCache::write_out(u64 index, Piece& piece)
{
    if (!piece.dirty)
        return {};
    u64 const first = index * (piece_size / Store::sector_size);
    u64 const sectors = min<u64>(piece_size / Store::sector_size, m_store.sector_count() - first);
    TRY(m_store.write_sectors(first, sectors, { piece.bytes, sectors * Store::sector_size }));
    piece.dirty = false;
    return {};
}

ErrorOr<void> SectorCache::make_room()
{
    if (m_pieces.size() < m_most_pieces)
        return {};
    u64 oldest_index = 0;
    u64 oldest = ~0ull;
    m_pieces.for_each([&](u64 index, Piece& piece) {
        if (piece.last_used < oldest) {
            oldest = piece.last_used;
            oldest_index = index;
        }
    });
    auto* piece = m_pieces.get(oldest_index);
    TRY(write_out(oldest_index, *piece));
    operator delete[](piece->bytes, Base::nothrow);
    m_pieces.remove(oldest_index);
    return {};
}

ErrorOr<SectorCache::Piece*> SectorCache::piece_at(u64 index, bool will_replace)
{
    if (auto* piece = m_pieces.get(index)) {
        piece->last_used = ++m_uses;
        return piece;
    }
    TRY(make_room());
    u64 const first = index * (piece_size / Store::sector_size);
    if (first >= m_store.sector_count())
        return Error::from_errno(EIO);
    auto* bytes = new (Base::nothrow) u8[piece_size];
    if (!bytes)
        return Error::from_errno(ENOMEM);
    __builtin_memset(bytes, 0, piece_size);
    if (!will_replace) {
        u64 const sectors = min<u64>(piece_size / Store::sector_size, m_store.sector_count() - first);
        auto read = m_store.read_sectors(first, sectors, { bytes, sectors * Store::sector_size });
        if (read.is_error()) {
            operator delete[](bytes, Base::nothrow);
            return read.release_error();
        }
    }
    Piece piece;
    piece.bytes = bytes;
    piece.last_used = ++m_uses;
    TRY(m_pieces.try_set(index, piece));
    return m_pieces.get(index);
}

ErrorOr<void> SectorCache::read(u64 offset, Bytes buffer)
{
    if (offset + buffer.size() > size())
        return Error::from_errno(EIO);
    size_t done = 0;
    while (done < buffer.size()) {
        u64 const at = offset + done;
        u64 const index = at / piece_size;
        size_t const inside = at % piece_size;
        size_t const part = min(buffer.size() - done, piece_size - inside);
        // A run of whole pieces nobody has here, straight from the store.
        if (inside == 0 && part == piece_size && !m_pieces.contains(index)) {
            size_t run = 1;
            size_t const most = (buffer.size() - done) / piece_size;
            while (run < most && !m_pieces.contains(index + run))
                ++run;
            TRY(m_store.read_sectors(index * (piece_size / Store::sector_size), run * (piece_size / Store::sector_size), { buffer.data() + done, run * piece_size }));
            done += run * piece_size;
            continue;
        }
        auto* piece = TRY(piece_at(index, false));
        __builtin_memcpy(buffer.data() + done, piece->bytes + inside, part);
        done += part;
    }
    return {};
}

ErrorOr<void> SectorCache::write(u64 offset, ReadonlyBytes buffer)
{
    if (offset + buffer.size() > size())
        return Error::from_errno(ENOSPC);
    size_t done = 0;
    while (done < buffer.size()) {
        u64 const at = offset + done;
        size_t const inside = at % piece_size;
        size_t const part = min(buffer.size() - done, piece_size - inside);
        // (Only the end of the store is ever less than a whole piece: that one is read first too.)
        bool const whole = part == piece_size;
        auto* piece = TRY(piece_at(at / piece_size, whole));
        __builtin_memcpy(piece->bytes + inside, buffer.data() + done, part);
        piece->dirty = true;
        done += part;
    }
    return {};
}

ErrorOr<void> SectorCache::zero(u64 offset, u64 count)
{
    if (offset + count > size())
        return Error::from_errno(ENOSPC);
    u64 done = 0;
    while (done < count) {
        u64 const at = offset + done;
        size_t const inside = at % piece_size;
        size_t const part = min<u64>(count - done, piece_size - inside);
        auto* piece = TRY(piece_at(at / piece_size, part == piece_size));
        __builtin_memset(piece->bytes + inside, 0, part);
        piece->dirty = true;
        done += part;
    }
    return {};
}

ErrorOr<void> SectorCache::flush()
{
    Vector<u64> dirty;
    ErrorOr<void> collecting {};
    m_pieces.for_each([&](u64 index, Piece& piece) {
        if (piece.dirty && !collecting.is_error())
            collecting = dirty.try_append(index);
    });
    TRY(collecting);
    // In order, which is kinder to whatever's underneath.
    quick_sort(Span<u64>(dirty.begin(), dirty.size()));
    for (auto index : dirty)
        TRY(write_out(index, *m_pieces.get(index)));
    return m_store.flush();
}

}
