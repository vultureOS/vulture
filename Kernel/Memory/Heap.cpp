/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Processor.h>
#include <LibBase/IntrusiveList.h>
#include <LibBase/New.h>

namespace Kernel {

static constexpr unsigned slab_order = 2;
static constexpr size_t slab_size = page_size << slab_order;
static constexpr unsigned smallest_size_shift = 4; // 16 bytes
// Empty slabs a cache keeps for reuse before giving memory back.
static constexpr size_t max_empty_slabs = 2;

class SlabCache;

struct FreeObject {
    FreeObject* next;
};

// The header at the start of every slab. Objects follow it, so no object
// starts at the beginning of a slab; kfree() relies on that.
struct Slab {
    IntrusiveListNode list_node;
    SlabCache* cache;
    FreeObject* free_list;
    u32 in_use;
    u32 capacity;
    u32 size_class;
};

static constexpr size_t slab_header_size = align_up<size_t>(sizeof(Slab), 16);

class SlabCache {
public:
    constexpr SlabCache(u32 size_class)
        : m_size_class(size_class)
        , m_object_size(1u << (size_class + smallest_size_shift))
    {
    }

    // Takes up to `count` objects. Returns how many it got: fewer only
    // when memory runs out.
    size_t allocate_batch(void** objects, size_t count);
    void free_batch(void* const* objects, size_t count);

    void add_statistics(HeapStatistics&) const;

private:
    Slab* create_slab();
    void free_object(void*);

    mutable SpinLock m_lock;
    u32 m_size_class;
    u32 m_object_size;
    IntrusiveList<Slab, &Slab::list_node> m_partial; // Some objects free.
    IntrusiveList<Slab, &Slab::list_node> m_full;
    IntrusiveList<Slab, &Slab::list_node> m_empty;
    size_t m_slab_count { 0 };
    size_t m_objects_in_use { 0 };
};

static SlabCache s_caches[heap_size_class_count] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static Atomic<size_t> s_large_pages { 0 };

static u8* to_pointer(PhysicalAddress address)
{
    return reinterpret_cast<u8*>(API::direct_map_base + address.get());
}

static PhysicalAddress to_physical(void const* pointer)
{
    auto address = reinterpret_cast<FlatPtr>(pointer);
    VERIFY(address >= API::direct_map_base);
    return PhysicalAddress(address - API::direct_map_base);
}

Slab* SlabCache::create_slab()
{
    auto block = physical_memory().allocate_pages(slab_order, PageUse::Slab);
    if (block.is_error())
        return nullptr;
    u8* base = to_pointer(block.value());
    auto* slab = new (base) Slab;
    slab->cache = this;
    slab->in_use = 0;
    slab->size_class = m_size_class;
    slab->capacity = static_cast<u32>((slab_size - slab_header_size) / m_object_size);
    // Thread the free list in address order.
    slab->free_list = nullptr;
    for (u32 i = slab->capacity; i > 0; --i) {
        auto* object = reinterpret_cast<FreeObject*>(base + slab_header_size + (i - 1) * m_object_size);
        object->next = slab->free_list;
        slab->free_list = object;
    }
    ++m_slab_count;
    return slab;
}

// A link in a free list, before it is followed: something else having written over a
// free object is only ever seen here, and what is around it is the best clue left.
static bool is_object_in_slab(Slab const* slab, FreeObject const* object, size_t object_size)
{
    size_t offset = reinterpret_cast<FlatPtr>(object) - reinterpret_cast<FlatPtr>(slab);
    return offset >= slab_header_size && offset < slab_size && (offset - slab_header_size) % object_size == 0;
}

[[noreturn]] static void report_corrupt_free_list(Slab const* slab, FreeObject const* holder, FreeObject const* link, size_t object_size)
{
    auto dump = [&](char const* what, void const* start, size_t bytes) {
        dmesgln("{} at {:p}:", what, start);
        auto const* words = static_cast<u64 const*>(start);
        for (size_t i = 0; i + 4 <= bytes / 8; i += 4)
            dmesgln("  {:016x} {:016x} {:016x} {:016x}", words[i], words[i + 1], words[i + 2], words[i + 3]);
    };
    dump("Slab header", slab, slab_header_size);
    if (holder) {
        auto const* bytes = reinterpret_cast<u8 const*>(holder);
        size_t words = min<size_t>(object_size, 128);
        if (reinterpret_cast<FlatPtr>(bytes) - reinterpret_cast<FlatPtr>(slab) >= slab_header_size + object_size)
            dump("The object before it", bytes - object_size, words);
        dump("The free object holding the link", bytes, words);
        if (reinterpret_cast<FlatPtr>(bytes) - reinterpret_cast<FlatPtr>(slab) + 2 * object_size <= slab_size)
            dump("The object after it", bytes + object_size, words);
    }
    FixedStringBuilder<96> reason;
    format_to(reason, "kmalloc: free list of {}-byte objects has {:p} in it", object_size, link);
    panic(reason.view());
}

size_t SlabCache::allocate_batch(void** objects, size_t count)
{
    SpinLockGuard guard(m_lock);
    size_t taken = 0;
    while (taken < count) {
        Slab* slab = m_partial.first();
        if (!slab) {
            slab = m_empty.take_first();
            if (!slab)
                slab = create_slab();
            if (!slab)
                break;
            m_partial.append(*slab);
        }
        FreeObject* holder = nullptr;
        while (taken < count && slab->free_list) {
            auto* object = slab->free_list;
            if (!is_object_in_slab(slab, object, m_object_size)) [[unlikely]]
                report_corrupt_free_list(slab, holder, object, m_object_size);
            slab->free_list = object->next;
            ++slab->in_use;
            objects[taken++] = object;
            holder = object;
        }
        if (!slab->free_list) {
            m_partial.remove(*slab);
            m_full.append(*slab);
        }
    }
    m_objects_in_use += taken;
    return taken;
}

void SlabCache::free_object(void* pointer)
{
    auto* slab = reinterpret_cast<Slab*>(align_down<FlatPtr>(reinterpret_cast<FlatPtr>(pointer), slab_size));
    VERIFY(slab->cache == this);
    // Catches pointers that are not the start of an object.
    VERIFY((reinterpret_cast<FlatPtr>(pointer) - reinterpret_cast<FlatPtr>(slab) - slab_header_size) % m_object_size == 0);
    VERIFY(slab->in_use > 0);

    bool was_full = slab->free_list == nullptr;
    auto* object = static_cast<FreeObject*>(pointer);
    object->next = slab->free_list;
    slab->free_list = object;
    --slab->in_use;

    if (was_full) {
        m_full.remove(*slab);
        m_partial.append(*slab);
    }
    if (slab->in_use == 0) {
        m_partial.remove(*slab);
        if (m_empty.size() < max_empty_slabs) {
            m_empty.append(*slab);
        } else {
            --m_slab_count;
            physical_memory().free_pages(to_physical(slab));
        }
    }
}

void SlabCache::free_batch(void* const* objects, size_t count)
{
    SpinLockGuard guard(m_lock);
    for (size_t i = 0; i < count; ++i)
        free_object(objects[i]);
    m_objects_in_use -= count;
}

void SlabCache::add_statistics(HeapStatistics& statistics) const
{
    SpinLockGuard guard(m_lock);
    statistics.slab_pages += m_slab_count << slab_order;
    statistics.small_objects_in_use += m_objects_in_use;
}

static size_t size_class_for(size_t size)
{
    if (size <= (1u << smallest_size_shift))
        return 0;
    // The smallest power of two that holds `size`.
    size_t shift = 64 - __builtin_clzll(size - 1);
    return shift - smallest_size_shift;
}

static unsigned order_for(size_t size)
{
    size_t pages = (size + page_size - 1) / page_size;
    unsigned order = 0;
    while ((1ull << order) < pages)
        ++order;
    return order;
}

void* kmalloc(size_t size)
{
    if (size == 0)
        size = 1;

    if (size > heap_largest_small_size) {
        unsigned order = order_for(size);
        if (order > BuddyAllocator::max_order)
            return nullptr;
        auto block = physical_memory().allocate_pages(order, PageUse::HeapLarge);
        if (block.is_error())
            return nullptr;
        s_large_pages.fetch_add(1ull << order, MemoryOrder::Relaxed);
        return to_pointer(block.value());
    }

    size_t size_class = size_class_for(size);
    InterruptDisabler disabler;
    auto& magazine = Processor::current().heap_cache().magazines[size_class];
    if (magazine.count == 0) {
        magazine.count = static_cast<u32>(s_caches[size_class].allocate_batch(magazine.objects, HeapProcessorCache::batch));
        if (magazine.count == 0)
            return nullptr;
    }
    return magazine.objects[--magazine.count];
}

void* kzalloc(size_t size)
{
    void* pointer = kmalloc(size);
    if (pointer)
        __builtin_memset(pointer, 0, size);
    return pointer;
}

void kfree(void* pointer)
{
    if (!pointer)
        return;
    auto address = to_physical(pointer);

    // Slabs are slab-aligned blocks tagged as such, and objects never start
    // at a slab's first byte. So if the slab-aligned block around the
    // pointer is a slab, the pointer is one of its objects; otherwise it
    // must be a large allocation.
    if (address.get() % slab_size != 0) {
        auto slab_address = PhysicalAddress(align_down<u64>(address.get(), slab_size));
        if (physical_memory().block_use(slab_address) == PageUse::Slab) {
            auto const* slab = reinterpret_cast<Slab const*>(to_pointer(slab_address));
            InterruptDisabler disabler;
            auto& magazine = Processor::current().heap_cache().magazines[slab->size_class];
            if (magazine.count == HeapProcessorCache::capacity) {
                magazine.count -= HeapProcessorCache::batch;
                s_caches[slab->size_class].free_batch(&magazine.objects[magazine.count], HeapProcessorCache::batch);
            }
            magazine.objects[magazine.count++] = pointer;
            return;
        }
    }

    if (!address.is_page_aligned() || physical_memory().block_use(address) != PageUse::HeapLarge) {
        FixedStringBuilder<96> reason;
        format_to(reason, "kfree: {:p} was not allocated by kmalloc", pointer);
        panic(reason.view());
    }
    auto order = physical_memory().free_pages(address);
    s_large_pages.fetch_sub(1ull << order, MemoryOrder::Relaxed);
}

HeapStatistics heap_statistics()
{
    HeapStatistics statistics;
    for (auto const& cache : s_caches)
        cache.add_statistics(statistics);
    statistics.large_pages = s_large_pages.load(MemoryOrder::Relaxed);
    return statistics;
}

void heap_drain_processor_cache()
{
    InterruptDisabler disabler;
    auto& cache = Processor::current().heap_cache();
    for (size_t size_class = 0; size_class < heap_size_class_count; ++size_class) {
        auto& magazine = cache.magazines[size_class];
        s_caches[size_class].free_batch(magazine.objects, magazine.count);
        magazine.count = 0;
    }
}

}

using Kernel::kfree;
using Kernel::kmalloc;

// Plain `new` cannot fail: a null result would be dereferenced by the
// constructor call. Running out of memory there is fatal; code that can
// cope uses kmalloc() directly.
void* operator new(size_t size)
{
    if (void* pointer = kmalloc(size))
        return pointer;
    Kernel::panic("operator new: out of memory"_sv);
}

void* operator new[](size_t size)
{
    return operator new(size);
}

void* operator new(size_t size, Base::NoThrowTag) noexcept
{
    return kmalloc(size);
}

void* operator new[](size_t size, Base::NoThrowTag) noexcept
{
    return kmalloc(size);
}

void operator delete(void* pointer, Base::NoThrowTag) noexcept
{
    kfree(pointer);
}

void operator delete[](void* pointer, Base::NoThrowTag) noexcept
{
    kfree(pointer);
}

void operator delete(void* pointer) noexcept
{
    kfree(pointer);
}

void operator delete(void* pointer, size_t) noexcept
{
    kfree(pointer);
}

void operator delete[](void* pointer) noexcept
{
    kfree(pointer);
}

void operator delete[](void* pointer, size_t) noexcept
{
    kfree(pointer);
}
