/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// The kernel heap. Small allocations come from slabs: 16 KiB blocks cut
// into equal objects, one cache per power-of-two size from 16 to 2048
// bytes. Each processor keeps a magazine of recently freed objects per size,
// so most calls never take a lock. Larger allocations are whole blocks from
// the physical memory manager.

namespace Kernel {

// 16-byte aligned. Returns null when memory runs out.
[[nodiscard]] void* kmalloc(size_t);
// Like kmalloc, but the memory is zeroed.
[[nodiscard]] void* kzalloc(size_t);
void kfree(void*);

constexpr size_t heap_size_class_count = 8;
constexpr size_t heap_largest_small_size = 2048;

// One per processor (see Processor::heap_cache()).
struct HeapProcessorCache {
    static constexpr u32 capacity = 32;
    static constexpr u32 batch = 16; // Objects moved per refill or flush.

    struct Magazine {
        u32 count { 0 };
        void* objects[capacity] {};
    };

    Magazine magazines[heap_size_class_count];
};

struct HeapStatistics {
    size_t slab_pages { 0 };
    size_t large_pages { 0 };
    // Handed out, or held in a processor's magazine.
    size_t small_objects_in_use { 0 };
};

[[nodiscard]] HeapStatistics heap_statistics();

// Returns this processor's magazines to the slabs.
void heap_drain_processor_cache();

}
