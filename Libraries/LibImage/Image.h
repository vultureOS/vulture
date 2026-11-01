/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/RefPtr.h>
#include <LibBase/Span.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>

// Image files, to and from bitmaps.

namespace Image {

// PNG (every color type, bit depth and interlacing) and JPEG (baseline and
// progressive), by what the data starts with.
[[nodiscard]] ErrorOr<NonnullRefPtr<Gfx::Bitmap>> decode(ReadonlyBytes);

[[nodiscard]] ErrorOr<NonnullRefPtr<Gfx::Bitmap>> decode_png(ReadonlyBytes);
[[nodiscard]] ErrorOr<NonnullRefPtr<Gfx::Bitmap>> decode_jpeg(ReadonlyBytes);
[[nodiscard]] ErrorOr<Vector<u8>> encode_png(Gfx::Bitmap const&);

// A .vicns icon file: the same icon drawn at several sizes and scales, each a
// PNG, so an icon stays sharp at each size it is shown.
//
//   "VICN", u32 version (1), u32 count (little endian), then per image
//   u32 points, u32 scale, u32 offset and u32 length, then the PNG data.
class IconFile {
public:
    struct Entry {
        u32 points { 0 };
        u32 scale { 0 };
        ReadonlyBytes data {};
    };

    // The bytes must outlive it.
    [[nodiscard]] static ErrorOr<IconFile> parse(ReadonlyBytes);

    [[nodiscard]] Vector<Entry> const& entries() const { return m_entries; }
    // The image that suits `points` at `scale` best: the smallest with at
    // least that many pixels, else the largest. Error if there are none.
    [[nodiscard]] ErrorOr<NonnullRefPtr<Gfx::Bitmap>> bitmap_for(u32 points, u32 scale) const;

private:
    Vector<Entry> m_entries;
};

// Builds a .vicns from (points, scale, PNG data) entries.
[[nodiscard]] ErrorOr<Vector<u8>> encode_icon_file(Vector<IconFile::Entry> const&);

}
