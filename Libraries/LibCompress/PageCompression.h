/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Span.h>
#include <LibBase/Types.h>

// Fast compression of one page of memory, for the kernel's memory compressor: it has to be quicker
// than the program waiting for it would notice, so this is a plain search for what repeats, with one
// guess at each position and nothing to be learned from the rest of the system.
//
// The stream is a list of operations, told apart by their first byte:
//
//   00nnnnnn                    n + 1 bytes follow, to be copied (1 to 64)
//   01nnnnnn oooooooo           copy n + 3 bytes from o + 1 back (3 to 66 bytes, up to 256 back)
//   1nnnnnnn oooooooo oooooooo  copy n + 4 bytes from o + 1 back (4 to 131 bytes); o is little-endian
//
// A copy may overlap what it is writing, which is how a run of something is said.

namespace Compress {

constexpr size_t compressible_page_size = 4096;

// What compress_page works in: too big for a kernel stack, and not worth allocating each time.
struct PageCompressionWorkspace {
    u16 positions[1024];
};

// Compresses the page into `output`. Returns the size, or 0 if it doesn't fit: and then stops as soon
// as it knows, so that what won't compress is cheap to find out about.
[[nodiscard]] size_t compress_page(u8 const* page, Bytes output, PageCompressionWorkspace&);

// Whether `input` was a whole page, which is then in `page`. Never reads or writes outside of them,
// whatever `input` says.
[[nodiscard]] bool decompress_page(ReadonlyBytes input, u8* page);

}
