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
#include <LibBase/Optional.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>

namespace Kernel {

class Inode;

// A contiguous, page-aligned range of a process's memory with one
// protection (PROT_* bits), backed by part of an AnonymousVMObject.
class Region {
public:
    Region(FlatPtr base, size_t size, int protection, StringView name, NonnullRefPtr<AnonymousVMObject>, size_t object_first_page);
    ~Region();
    Region(Region const&) = delete;
    Region& operator=(Region const&) = delete;

    [[nodiscard]] FlatPtr base() const { return m_base; }
    [[nodiscard]] size_t size() const { return m_size; }
    [[nodiscard]] FlatPtr end() const { return m_base + m_size; }
    [[nodiscard]] int protection() const { return m_protection; }
    [[nodiscard]] StringView name() const { return { m_name, m_name_length }; }
    [[nodiscard]] AnonymousVMObject& object() { return *m_object; }
    [[nodiscard]] bool contains(FlatPtr address) const { return address >= m_base && address < end(); }
    // The object page behind a page of this region.
    [[nodiscard]] size_t object_index(FlatPtr address) const { return m_object_first_page + (address - m_base) / page_size; }

    // A file mapping takes its pages from the file: object page i is file
    // page i + delta. Shared ones write to the file's own pages; private
    // ones copy a page on its first write.
    void set_file(Inode&, i64 page_delta, bool shared, bool writer);
    [[nodiscard]] Inode* file() const { return m_file.ptr(); }
    [[nodiscard]] bool is_shared() const { return m_shared; }
    [[nodiscard]] u64 file_page(size_t object_index) const { return static_cast<u64>(static_cast<i64>(object_index) + m_file_page_delta); }

private:
    friend class AddressSpace;

    FlatPtr m_base;
    size_t m_size;
    int m_protection;
    char m_name[32] {};
    size_t m_name_length { 0 };
    NonnullRefPtr<AnonymousVMObject> m_object;
    size_t m_object_first_page;
    RefPtr<Inode> m_file;
    i64 m_file_page_delta { 0 };
    bool m_shared { false };
    bool m_writer { false }; // Counted with the file as a shared writer.
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
//
// Its process owns it; anyone else looking in (Process::describe) holds a reference
// for as long as that takes, so that exec and exit cannot free it under them.
class AddressSpace final : public RefCounted<AddressSpace> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<AddressSpace>> create();
    ~AddressSpace();

    // Maps anonymous memory. With `fixed`, exactly at `address` (replacing
    // what was there); otherwise `address` is a hint and 0 means anywhere.
    // Writable and executable at once is refused (W^X).
    ErrorOr<Region*> map_anonymous(FlatPtr address, size_t size, int protection, StringView name, bool fixed);
    // Maps part of a file, from `offset` (page-aligned). `writer`: shared,
    // and the file was opened for writing.
    ErrorOr<Region*> map_file(FlatPtr address, size_t size, int protection, bool fixed, Inode&, u64 offset, bool shared, bool writer);
    ErrorOr<void> unmap(FlatPtr address, size_t size);
    // Unmaps everything. For a dying process, from its own thread: ending
    // a file mapping may sleep, which the scheduler's reaper (where the
    // rest of the address space goes) must not.
    void remove_all_regions();
    ErrorOr<void> protect(FlatPtr address, size_t size, int protection);

    PageFaultResult handle_fault(FlatPtr address, AccessKind);

    // For fork: a copy that shares every page copy-on-write.
    [[nodiscard]] ErrorOr<NonnullRefPtr<AddressSpace>> clone_for_fork();

    // Makes this the current processor's user space.
    void activate() const;

    [[nodiscard]] Region* find_region(FlatPtr address);

    // The file (and where in it) that `address` is mapped from, if any:
    // what crash reports name a library frame by.
    struct FileLocation {
        RefPtr<Inode> file;
        u64 offset { 0 };
    };
    [[nodiscard]] Optional<FileLocation> file_location(FlatPtr address);

    // Out-of-line memory in port messages: the pages of [address, address +
    // size) (within one region) as a new object sharing them copy-on-write;
    // and an object mapped at a free address.
    ErrorOr<NonnullRefPtr<AnonymousVMObject>> copy_range(FlatPtr address, size_t size);
    ErrorOr<FlatPtr> map_object(NonnullRefPtr<AnonymousVMObject>, size_t size, int protection, StringView name);
    // Pages the program has touched (some may be shared).
    [[nodiscard]] size_t resident_pages();
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
    ErrorOr<Region*> create_region(FlatPtr address, size_t size, int protection, StringView name, bool fixed);
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
