/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Arch/PageTable.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Memory/AnonymousVMObject.h>
#include <LibBase/IntrusiveList.h>
#include <LibBase/OwnPtr.h>

namespace Kernel {

// A contiguous, page-aligned range of a process's memory with one
// protection (PROT_* bits), backed by part of an AnonymousVMObject.
class Region {
public:
    Region(FlatPtr base, size_t size, int protection, StringView name, NonnullRefPtr<AnonymousVMObject>, size_t object_first_page);

    [[nodiscard]] FlatPtr base() const { return m_base; }
    [[nodiscard]] size_t size() const { return m_size; }
    [[nodiscard]] FlatPtr end() const { return m_base + m_size; }
    [[nodiscard]] int protection() const { return m_protection; }
    [[nodiscard]] StringView name() const { return { m_name, m_name_length }; }
    [[nodiscard]] AnonymousVMObject& object() { return *m_object; }
    [[nodiscard]] bool contains(FlatPtr address) const { return address >= m_base && address < end(); }
    // The object page behind a page of this region.
    [[nodiscard]] size_t object_index(FlatPtr address) const { return m_object_first_page + (address - m_base) / page_size; }

private:
    friend class AddressSpace;

    FlatPtr m_base;
    size_t m_size;
    int m_protection;
    char m_name[32] {};
    size_t m_name_length { 0 };
    NonnullRefPtr<AnonymousVMObject> m_object;
    size_t m_object_first_page;
    IntrusiveListNode m_list_node;
};

enum class AccessKind : u8 {
    Read,
    Write,
    Execute,
};

enum class PageFaultResult : u8 {
    Resolved,
    BadAccess, // Nothing mapped there, or not with that access.
    OutOfMemory,
};

// A process's user space: its regions and the page table that maps them.
// Pages are mapped lazily, by the page fault handler.
class AddressSpace {
public:
    [[nodiscard]] static ErrorOr<NonnullOwnPtr<AddressSpace>> create();
    ~AddressSpace();

    // Maps anonymous memory. With `fixed`, exactly at `address` (replacing
    // what was there); otherwise `address` is a hint and 0 means anywhere.
    // Writable and executable at once is refused (W^X).
    ErrorOr<Region*> map_anonymous(FlatPtr address, size_t size, int protection, StringView name, bool fixed);
    ErrorOr<void> unmap(FlatPtr address, size_t size);
    ErrorOr<void> protect(FlatPtr address, size_t size, int protection);

    PageFaultResult handle_fault(FlatPtr address, AccessKind);

    // For fork: a copy that shares every page copy-on-write.
    [[nodiscard]] ErrorOr<NonnullOwnPtr<AddressSpace>> clone_for_fork();

    // Makes this the current processor's user space.
    void activate() const;

    [[nodiscard]] Region* find_region(FlatPtr address);
    [[nodiscard]] Arch::PageTable const& page_table() const { return m_page_table; }
    [[nodiscard]] u16 asid() const { return m_asid; }

    template<typename Callback>
    void for_each_region(Callback callback)
    {
        for (auto& region : m_regions)
            callback(region);
    }

private:
    AddressSpace(Arch::PageTable, u16 asid);

    ErrorOr<FlatPtr> find_free_range(FlatPtr hint, size_t size) const;
    // Removes [start, end) of `region` from the page table and releases
    // the pages behind it.
    void release_range(Region&, FlatPtr start, FlatPtr end);
    void insert_sorted(Region&);
    ErrorOr<void> split_around(FlatPtr address, size_t size);

    Mutex m_lock;
    Arch::PageTable m_page_table;
    u16 m_asid { 0 };
    IntrusiveList<Region, &Region::m_list_node> m_regions;
};

}
