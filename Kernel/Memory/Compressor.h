/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// Memory that hasn't been used for a while is compressed, when memory is needed for something else.
// There is no swap; this is what stands in for it, and it is how the same amount of memory goes further.
//
// A thread keeps an eye on free memory, and when that runs low goes round the address spaces like a
// clock. A page it finds mapped, it unmaps: if the program is using it, the next touch only maps it
// again. One it finds still unmapped the next time round hasn't been touched since, and goes into the
// pool here, in as little room as it will fit, leaving its object with a handle to it
// (AnonymousVMObject). A page of nothing but zeros is simply let go, as an untouched page is zeros
// anyway. Touching any of them again is a page fault that gets the page back.
//
// What isn't compressed: pages from files, which aren't anonymous memory; pages shared with another
// process until one of them has its own copy; and what won't get to half its size.
//
// Compressed pages are shared by fork, as the pages themselves are: each process that touches one
// decompresses its own copy.
//
// A fault that needs a page when there are none waits for this thread. When there's truly nothing left
// to compress, the process using the most memory is ended, as its memory is what would help most; a
// process that is only unlucky enough to want a page just then is left alone.

namespace Kernel {

class AddressSpace;

namespace Compressor {

// A page in the pool. Holds a reference.
using Handle = void*;

struct Statistics {
    u64 pages { 0 };          // Pages held compressed, right now.
    u64 pool_bytes { 0 };     // The memory that is taking up.
    u64 stored_bytes { 0 };   // How much of it is their contents.
    u64 compressions { 0 };   // Since boot.
    u64 decompressions { 0 }; // Since boot.
    u64 zero_pages { 0 };     // Let go of, since boot.
    u64 rejected { 0 };       // Wouldn't compress, since boot.
};

void initialize();

// A copy of the page in the pool; null if it wouldn't be worth it.
[[nodiscard]] Handle store(u8 const* page);
// Back into a page. The handle stays good.
void load(Handle, u8* page);
void ref(Handle);
void unref(Handle);

void note_zero_page();
[[nodiscard]] Statistics statistics();

// From the allocator, when free memory gets low. Any context.
void memory_is_low();

// From a page fault that couldn't get a page, holding no locks: waits for this thread to do some more.
// False if there's nothing left that it can do, not even ending something.
[[nodiscard]] bool wait_for_memory();

}

}
