/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/CPU.h>
#include <Kernel/Security/Random.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/memory.h>
#include <Kernel/Arch/MemoryLayout.h>
#include <Kernel/Arch/UserContext.h>
#include <Kernel/FileSystem/Inode.h>
#include <Kernel/Memory/AddressSpace.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <LibBase/New.h>

namespace Kernel {

using Arch::Protection;

Region::Region(FlatPtr base, size_t size, int protection, StringView name, NonnullRefPtr<AnonymousVMObject> object, size_t object_first_page)
    : m_base(base)
    , m_size(size)
    , m_protection(protection)
    , m_object(move(object))
    , m_object_first_page(object_first_page)
{
    m_name_length = min(name.length(), sizeof(m_name));
    __builtin_memcpy(m_name, name.characters_without_null_termination(), m_name_length);
}

Region::~Region()
{
    if (m_writer)
        m_file->remove_shared_writer();
}

void Region::set_file(Inode& file, i64 page_delta, bool shared, bool writer)
{
    m_file = file;
    m_file_page_delta = page_delta;
    m_shared = shared;
    m_writer = writer;
    if (writer)
        file.add_shared_writer();
}

static bool is_page_aligned(FlatPtr value)
{
    return value % page_size == 0;
}

static bool is_valid_user_range(FlatPtr address, size_t size)
{
    FlatPtr end;
    return size > 0 && is_page_aligned(address) && is_page_aligned(size) && !__builtin_add_overflow(address, size, &end)
        && address >= MemoryLayout::user_space_start && end <= MemoryLayout::user_space_end;
}

// How a page of a region is mapped. Writable only if the page is not
// shared: a shared page is mapped read-only and copied on the first write.
static Protection protection_for(int region_protection, bool may_write)
{
    if (region_protection & PROT_EXEC)
        return Protection::ReadExecute;
    if ((region_protection & PROT_WRITE) && may_write)
        return Protection::ReadWrite;
    return Protection::ReadOnly;
}

AddressSpace::AddressSpace(Arch::PageTable page_table, u16 asid)
    : m_page_table(page_table)
    , m_asid(asid)
{
}

ErrorOr<NonnullRefPtr<AddressSpace>> AddressSpace::create()
{
    auto page_table = TRY(Arch::PageTable::create());
    auto asid = Arch::allocate_address_space_id();
    if (asid.is_error()) {
        page_table.destroy();
        return asid.release_error();
    }
    auto* space = new (Base::nothrow) AddressSpace(page_table, asid.value());
    if (!space) {
        Arch::free_address_space_id(asid.value());
        page_table.destroy();
        return Error::from_errno(ENOMEM);
    }
    u64 random;
    get_random_bytes({ reinterpret_cast<u8*>(&random), sizeof(random) });
    space->m_mmap_base = MemoryLayout::user_mmap_base + (random % MemoryLayout::user_mmap_random_pages) * page_size;
    get_random_bytes({ reinterpret_cast<u8*>(space->m_pointer_key), sizeof(space->m_pointer_key) });
    return adopt_ref(*space);
}

AddressSpace::~AddressSpace()
{
    // Nothing runs on this address space any more. Dropping the ASID
    // flushes its TLB entries everywhere, so the tables can go.
    Arch::free_address_space_id(m_asid);
    m_page_table.destroy();
    while (auto* region = m_regions.take_first())
        delete region;
}

void AddressSpace::activate() const
{
    Arch::activate_user_page_table(&m_page_table, m_asid);
    Arch::set_user_pointer_key(m_pointer_key[0], m_pointer_key[1]);
}

Region* AddressSpace::find_region(FlatPtr address)
{
    for (auto& region : m_regions) {
        if (region.contains(address))
            return &region;
        if (region.base() > address)
            break;
    }
    return nullptr;
}

void AddressSpace::insert_sorted(Region& region)
{
    for (auto& existing : m_regions) {
        if (existing.base() > region.base()) {
            m_regions.insert_before(region, existing);
            return;
        }
    }
    m_regions.append(region);
}

ErrorOr<FlatPtr> AddressSpace::find_free_range(FlatPtr hint, size_t size) const
{
    FlatPtr candidate = max(hint, m_mmap_base);
    // Keep clear of the stack area at the top.
    FlatPtr limit = MemoryLayout::user_stack_top - MemoryLayout::user_stack_size - page_size;
    for (auto& region : m_regions) {
        if (region.end() <= candidate)
            continue;
        if (region.base() >= candidate + size)
            break;
        candidate = region.end();
    }
    if (candidate + size > limit || candidate + size < candidate)
        return Error::from_errno(ENOMEM);
    return candidate;
}

ErrorOr<Region*> AddressSpace::map_anonymous(FlatPtr address, size_t size, int protection, StringView name, bool fixed)
{
    MutexLocker locker(m_lock);
    return create_region(address, size, protection, name, fixed);
}

ErrorOr<Region*> AddressSpace::map_file(FlatPtr address, size_t size, int protection, bool fixed, Inode& file, u64 offset, bool shared, bool writer)
{
    if (offset % page_size != 0)
        return Error::from_errno(EINVAL);
    MutexLocker locker(m_lock);
    auto* region = TRY(create_region(address, size, protection, shared ? "shared file"_sv : "file"_sv, fixed));
    region->set_file(file, static_cast<i64>(offset / page_size), shared, writer);
    return region;
}

ErrorOr<Region*> AddressSpace::create_region(FlatPtr address, size_t size, int protection, StringView name, bool fixed)
{
    size = align_up<size_t>(size, page_size);
    if ((protection & PROT_WRITE) && (protection & PROT_EXEC))
        return Error::from_errno(EACCES);
    if (fixed) {
        if (!is_valid_user_range(address, size))
            return Error::from_errno(EINVAL);
        // Whatever was there goes.
        TRY(split_around(address, size));
        for (auto* region = m_regions.first(); region;) {
            auto* next = m_regions.next(*region);
            if (region->base() >= address && region->end() <= address + size) {
                release_range(*region, region->base(), region->end());
                m_regions.remove(*region);
                delete region;
            }
            region = next;
        }
    } else {
        if (size == 0)
            return Error::from_errno(EINVAL);
        address = TRY(find_free_range(is_page_aligned(address) ? address : 0, size));
    }
    auto object = TRY(AnonymousVMObject::create(size / page_size));
    auto* region = new (Base::nothrow) Region(address, size, protection, name, move(object), 0);
    if (!region)
        return Error::from_errno(ENOMEM);
    insert_sorted(*region);
    return region;
}

// Splits regions so that none straddles `address` or `address + size`.
ErrorOr<void> AddressSpace::split_around(FlatPtr address, size_t size)
{
    FlatPtr const boundaries[] = { address, address + size };
    for (FlatPtr boundary : boundaries) {
        auto* region = find_region(boundary);
        if (region && region->base() != boundary) {
            auto* tail = new (Base::nothrow) Region(boundary, region->end() - boundary, region->protection(), region->name(), region->m_object, region->object_index(boundary));
            if (!tail)
                return Error::from_errno(ENOMEM);
            if (region->m_file)
                tail->set_file(*region->m_file, region->m_file_page_delta, region->m_shared, region->m_writer);
            region->m_size = boundary - region->base();
            insert_sorted(*tail);
        }
    }
    return {};
}

void AddressSpace::release_range(Region& region, FlatPtr start, FlatPtr end)
{
    for (FlatPtr page = start; page < end; page += page_size) {
        (void)m_page_table.unmap_user_page(page);
        region.object().release_page(region.object_index(page));
    }
}

ErrorOr<void> AddressSpace::unmap(FlatPtr address, size_t size)
{
    size = align_up<size_t>(size, page_size);
    if (!is_valid_user_range(address, size))
        return Error::from_errno(EINVAL);
    MutexLocker locker(m_lock);
    TRY(split_around(address, size));
    for (auto* region = m_regions.first(); region;) {
        auto* next = m_regions.next(*region);
        if (region->base() >= address && region->end() <= address + size) {
            release_range(*region, region->base(), region->end());
            m_regions.remove(*region);
            delete region;
        }
        region = next;
    }
    return {};
}

Optional<AddressSpace::FileLocation> AddressSpace::file_location(FlatPtr address)
{
    MutexLocker locker(m_lock);
    auto* region = find_region(address);
    if (!region || !region->file())
        return {};
    FlatPtr page = align_down<FlatPtr>(address, page_size);
    return FileLocation { region->file(), region->file_page(region->object_index(page)) * page_size + (address - page) };
}

ErrorOr<NonnullRefPtr<AnonymousVMObject>> AddressSpace::copy_range(FlatPtr address, size_t size)
{
    if (!is_page_aligned(address) || size == 0 || size % page_size != 0 || !is_valid_user_range(address, size))
        return Error::from_errno(EINVAL);
    MutexLocker locker(m_lock);
    auto* region = find_region(address);
    if (!region || address + size > region->end())
        return Error::from_errno(EFAULT);
    // A file mapping's untouched pages are not in its object yet (they
    // come from the file on the first touch): only anonymous memory.
    if (region->file())
        return Error::from_errno(EINVAL);
    auto object = TRY(region->object().clone_range(region->object_index(address), size / page_size));
    // Shared now: our writable pages must fault on the next write, as
    // after fork().
    if ((region->protection() & PROT_WRITE) && !region->is_shared()) {
        for (FlatPtr page = address; page < address + size; page += page_size) {
            if (region->object().page(region->object_index(page)).get())
                m_page_table.protect_user_page(page, protection_for(region->protection(), false));
        }
    }
    return object;
}

ErrorOr<FlatPtr> AddressSpace::map_object(NonnullRefPtr<AnonymousVMObject> object, size_t size, int protection, StringView name)
{
    if (size == 0 || size % page_size != 0 || ((protection & PROT_WRITE) && (protection & PROT_EXEC)))
        return Error::from_errno(EINVAL);
    MutexLocker locker(m_lock);
    FlatPtr address = TRY(find_free_range(0, size));
    auto* region = new (Base::nothrow) Region(address, size, protection, name, move(object), 0);
    if (!region)
        return Error::from_errno(ENOMEM);
    insert_sorted(*region);
    return address;
}

size_t AddressSpace::resident_pages()
{
    MutexLocker locker(m_lock);
    size_t count = 0;
    for (auto& region : m_regions) {
        for (FlatPtr page = region.base(); page < region.end(); page += page_size) {
            if (region.object().page(region.object_index(page)).get())
                ++count;
        }
    }
    return count;
}

void AddressSpace::remove_all_regions()
{
    MutexLocker locker(m_lock);
    while (auto* region = m_regions.take_first()) {
        release_range(*region, region->base(), region->end());
        delete region;
    }
}

ErrorOr<void> AddressSpace::protect(FlatPtr address, size_t size, int protection)
{
    size = align_up<size_t>(size, page_size);
    if (!is_valid_user_range(address, size))
        return Error::from_errno(EINVAL);
    if ((protection & PROT_WRITE) && (protection & PROT_EXEC))
        return Error::from_errno(EACCES);
    MutexLocker locker(m_lock);
    // The whole range must be mapped.
    for (FlatPtr page = address; page < address + size; page += page_size) {
        if (!find_region(page))
            return Error::from_errno(ENOMEM);
    }
    TRY(split_around(address, size));
    for (auto& region : m_regions) {
        if (region.base() < address || region.end() > address + size)
            continue;
        bool becomes_code = (protection & PROT_EXEC) && !(region.m_protection & PROT_EXEC);
        region.m_protection = protection;
        for (FlatPtr page = region.base(); page < region.end(); page += page_size) {
            auto physical = region.object().page(region.object_index(page));
            if (!physical.get())
                continue;
            if (becomes_code)
                Arch::make_code_coherent(physical);
            if (protection == PROT_NONE) {
                (void)m_page_table.unmap_user_page(page);
                continue;
            }
            bool exclusive = region.is_shared() || physical_memory().user_page_references(physical) == 1;
            if (!m_page_table.protect_user_page(page, protection_for(protection, exclusive)))
                continue;
        }
    }
    return {};
}

PageFaultResult AddressSpace::handle_fault(FlatPtr address, AccessKind access)
{
    MutexLocker locker(m_lock);
    auto* region = find_region(address);
    if (!region)
        return PageFaultResult::BadAccess;
    int protection = region->protection();
    bool allowed = (access == AccessKind::Read && (protection & (PROT_READ | PROT_WRITE | PROT_EXEC)))
        || (access == AccessKind::Write && (protection & PROT_WRITE))
        || (access == AccessKind::Execute && (protection & PROT_EXEC));
    if (!allowed)
        return PageFaultResult::BadAccess;

    FlatPtr page_address = align_down<FlatPtr>(address, page_size);
    size_t index = region->object_index(page_address);
    auto& object = region->object();
    auto physical = object.page(index);

    if (!physical.get() && region->file()) {
        // First touch of a file mapping: the file's page, shared with its
        // cache (a private mapping copies it on the first write, below).
        auto page = region->file()->mapped_page(region->file_page(index));
        if (page.is_error()) {
            // Past the end of a device's memory: a bad access by the program.
            if (page.error().is_errno() && page.error().code() == ENXIO)
                return PageFaultResult::BadAccess;
            return PageFaultResult::OutOfMemory;
        }
        object.set_page(index, page.value());
        physical = page.value();
    } else if (!physical.get()) {
        // First touch: a fresh zeroed page.
        auto page = physical_memory().allocate_user_page();
        if (page.is_error())
            return PageFaultResult::OutOfMemory;
        object.set_page(index, page.value());
        if (m_page_table.map_user_page(page_address, page.value(), protection_for(protection, true)).is_error())
            return PageFaultResult::OutOfMemory;
        return PageFaultResult::Resolved;
    }

    // Code from a file (a library's text) was written through the data
    // cache; instruction fetches must see it.
    if (protection & PROT_EXEC)
        Arch::make_code_coherent(physical);

    bool exclusive = region->is_shared() || physical_memory().user_page_references(physical) == 1;
    if (access == AccessKind::Write && !exclusive) {
        // Copy on write: this address space gets a page of its own.
        auto copy = physical_memory().allocate_user_page();
        if (copy.is_error())
            return PageFaultResult::OutOfMemory;
        __builtin_memcpy(physical_to_virtual(copy.value()), physical_to_virtual(physical), page_size);
        object.set_page(index, copy.value());
        physical = copy.value();
        exclusive = true;
    }
    if (m_page_table.map_user_page(page_address, physical, protection_for(protection, exclusive)).is_error())
        return PageFaultResult::OutOfMemory;
    return PageFaultResult::Resolved;
}

ErrorOr<NonnullRefPtr<AddressSpace>> AddressSpace::clone_for_fork()
{
    auto child = TRY(create());
    child->m_mmap_base = m_mmap_base;
    child->m_pointer_key[0] = m_pointer_key[0];
    child->m_pointer_key[1] = m_pointer_key[1];
    MutexLocker locker(m_lock);
    for (auto& region : m_regions) {
        auto object = TRY(region.object().clone_range(region.object_index(region.base()), region.size() / page_size));
        auto* copy = new (Base::nothrow) Region(region.base(), region.size(), region.protection(), region.name(), move(object), 0);
        if (!copy)
            return Error::from_errno(ENOMEM);
        child->m_regions.append(*copy);
        if (region.m_file)
            copy->set_file(*region.m_file, region.m_file_page_delta + static_cast<i64>(region.object_index(region.base())), region.m_shared, region.m_writer);
        // Now shared: our writable pages must fault on the next write
        // (but a shared mapping writes the same pages in both).
        if ((region.protection() & PROT_WRITE) && !region.is_shared()) {
            for (FlatPtr page = region.base(); page < region.end(); page += page_size) {
                if (region.object().page(region.object_index(page)).get())
                    m_page_table.protect_user_page(page, protection_for(region.protection(), false));
            }
        }
    }
    return child;
}

}
