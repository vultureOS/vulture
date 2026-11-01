/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Storage/BlockDevice.h>
#include <Kernel/FileSystem/VultureFS.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Time/Time.h>
#include <LibBase/HashMap.h>
#include <LibBase/New.h>
#include <LibBase/QuickSort.h>
#include <LibVultureFS/Volume.h>

namespace Kernel {

using VultureFS::InodeNumber;
using VultureFS::InodeRecord;

namespace {

// Pages read from the disk at once on a miss.
constexpr size_t read_ahead_pages = 16;
// Contiguous dirty pages written at once.
constexpr size_t write_back_pages = 32;
// Past this many dirty pages, a writer syncs before going on.
constexpr size_t dirty_page_limit = 8192;
// Past this many cached pages, files nobody has open are forgotten.
constexpr size_t cached_page_limit = 32768;
constexpr size_t cached_inode_limit = 4096;

static_assert(VultureFS::block_size == page_size);

i64 kernel_clock()
{
    return static_cast<i64>(Time::wall_clock_nanoseconds());
}

InodeType type_of(InodeRecord const& record)
{
    switch (record.mode & VultureFS::mode_type_mask) {
    case VultureFS::mode_directory:
        return InodeType::Directory;
    case VultureFS::mode_symbolic_link:
        return InodeType::SymbolicLink;
    default:
        return InodeType::File;
    }
}

// The volume's 4 KiB blocks on a partition of 512-byte sectors.
class PartitionStore final : public VultureFS::BlockStore {
public:
    explicit PartitionStore(BlockDevice& device)
        : m_device(device)
    {
    }

    [[nodiscard]] u64 block_count() const override { return m_device->block_count() / sectors_per_block; }

    ErrorOr<void> read_blocks(VultureFS::BlockIndex first, size_t count, Bytes buffer) override
    {
        return m_device->read_blocks(first * sectors_per_block, count * sectors_per_block, buffer.trim(count * VultureFS::block_size));
    }

    ErrorOr<void> write_blocks(VultureFS::BlockIndex first, size_t count, ReadonlyBytes buffer) override
    {
        return m_device->write_blocks(first * sectors_per_block, count * sectors_per_block, buffer.trim(count * VultureFS::block_size));
    }

    ErrorOr<void> flush() override { return m_device->flush(); }

private:
    static constexpr u64 sectors_per_block = VultureFS::block_size / BlockDevice::block_size;
    NonnullRefPtr<BlockDevice> m_device;
};

}

class VultureFSInode;

class VultureFileSystem final : public FileSystem {
public:
    static ErrorOr<NonnullRefPtr<VultureFileSystem>> create(BlockDevice&);

    [[nodiscard]] StringView name() const override { return "vultureFS"_sv; }
    [[nodiscard]] StringView device_name() const override { return m_device_name.view(); }
    [[nodiscard]] Statistics statistics() override
    {
        MutexLocker locker(m_lock);
        auto statistics = m_volume->statistics();
        return { VultureFS::block_size, statistics.block_count, statistics.free_blocks };
    }
    [[nodiscard]] NonnullRefPtr<Inode> root_inode() override;
    ErrorOr<void> sync() override;
    ErrorOr<void> rename(Inode& old_parent, StringView old_name, Inode& new_parent, StringView new_name) override;

    [[nodiscard]] Mutex& lock() { return m_lock; }
    [[nodiscard]] VultureFS::Volume& volume() { return *m_volume; }

    // These want the lock held.
    ErrorOr<NonnullRefPtr<VultureFSInode>> inode(InodeNumber);
    // Re-reads a cached inode's record after the volume changed it.
    void refresh(InodeNumber);
    // A name went away; an inode left with none is deleted once unused.
    ErrorOr<void> unlinked(VultureFS::Unlinked);
    void count_pages(i64 cached, i64 dirty)
    {
        m_cached_pages = static_cast<size_t>(static_cast<i64>(m_cached_pages) + cached);
        m_dirty_pages = static_cast<size_t>(static_cast<i64>(m_dirty_pages) + dirty);
    }
    [[nodiscard]] bool has_too_many_dirty_pages() const { return m_dirty_pages > dirty_page_limit; }
    ErrorOr<void> sync_locked();

private:
    explicit VultureFileSystem(BlockDevice& device)
        : m_store(device)
        , m_device_name(MUST(String::from_view(device.name())))
    {
    }

    void forget_unused_inodes();

    PartitionStore m_store;
    String m_device_name;
    OwnPtr<VultureFS::Volume> m_volume;
    Mutex m_lock;
    HashMap<InodeNumber, RefPtr<VultureFSInode>> m_inodes;
    size_t m_cached_pages { 0 };
    size_t m_dirty_pages { 0 };
};

// A file, directory or symbolic link. There is one per inode number at a
// time (the file system's cache holds them), so its pages are the only
// copy of the file's data in memory.
class VultureFSInode final : public Inode {
public:
    VultureFSInode(VultureFileSystem& file_system, InodeNumber number, InodeRecord const& record)
        : Inode(file_system, number)
        , m_record(record)
    {
    }

    ~VultureFSInode() override
    {
        m_pages.for_each([&](u64, CachedPage& page) {
            physical_memory().unref_user_page(page.address);
        });
        fs().count_pages(-static_cast<i64>(m_pages.size()), -static_cast<i64>(m_dirty_count));
    }

    [[nodiscard]] InodeMetadata metadata() const override
    {
        MutexLocker locker(fs().lock());
        return metadata_locked();
    }

    ErrorOr<size_t> read(u64 offset, size_t count, IOBuffer const& buffer) override;
    ErrorOr<size_t> write(u64 offset, size_t count, IOBuffer const& buffer) override;
    ErrorOr<void> truncate(u64 size) override;
    ErrorOr<NonnullRefPtr<Inode>> lookup(StringView name) override;
    ErrorOr<void> traverse(DirectoryVisitor&) override;
    ErrorOr<NonnullRefPtr<Inode>> create_child(StringView name, InodeType, u32 permissions, StringView link_target) override;
    ErrorOr<void> remove_child(StringView name) override;
    ErrorOr<void> add_child(StringView name, Inode&) override;
    ErrorOr<size_t> read_link(Bytes buffer) override;
    ErrorOr<void> set_permissions(u32 permissions) override;
    ErrorOr<void> set_owner(u32 uid, u32 gid) override;
    ErrorOr<void> set_times(i64 access_time, i64 modification_time) override;
    ErrorOr<void> sync() override { return fs().sync(); }
    ErrorOr<PhysicalAddress> mapped_page(u64 index) override;
    void add_shared_writer() override;
    void remove_shared_writer() override;

    // These want the file system's lock held.
    void set_record(InodeRecord record)
    {
        // A file's in-memory size runs ahead of the volume's until its
        // pages are written back.
        if (type_of(record) == InodeType::File)
            record.size = m_record.size;
        m_record = record;
    }
    [[nodiscard]] InodeRecord const& record() const { return m_record; }
    // Shared writable mappings can change any page unseen, so while there
    // are some, every page counts as dirty.
    [[nodiscard]] bool is_dirty() const { return m_dirty_count > 0 || m_times_dirty || m_shared_writers > 0; }
    ErrorOr<void> write_back();

private:
    struct CachedPage {
        PhysicalAddress address;
        bool dirty { false };
    };

    [[nodiscard]] VultureFileSystem& fs() const { return static_cast<VultureFileSystem&>(file_system()); }
    [[nodiscard]] InodeMetadata metadata_locked() const;
    // The page holding file page `index`, referenced for the caller: from
    // the cache, else read from the disk (with the pages after it) if
    // `load`, else zeroed.
    ErrorOr<PhysicalAddress> page(u64 index, bool load);
    ErrorOr<void> read_ahead(u64 index);
    void drop_pages_from(u64 first);
    void mark_dirty(u64 index, PhysicalAddress);
    void mark_all_dirty();

    InodeRecord m_record;
    HashMap<u64, CachedPage> m_pages;
    size_t m_dirty_count { 0 };
    size_t m_shared_writers { 0 };
    bool m_times_dirty { false };
};

ErrorOr<NonnullRefPtr<VultureFileSystem>> VultureFileSystem::create(BlockDevice& device)
{
    auto* raw = new (Base::nothrow) VultureFileSystem(device);
    if (!raw)
        return Error::from_errno(ENOMEM);
    auto file_system = adopt_ref(*raw);
    file_system->m_volume = TRY(VultureFS::Volume::mount(file_system->m_store, kernel_clock));
    return file_system;
}

NonnullRefPtr<Inode> VultureFileSystem::root_inode()
{
    MutexLocker locker(m_lock);
    return MUST(inode(VultureFS::root_directory));
}

ErrorOr<NonnullRefPtr<VultureFSInode>> VultureFileSystem::inode(InodeNumber number)
{
    if (auto* cached = m_inodes.get(number))
        return NonnullRefPtr<VultureFSInode>(**cached);
    auto record = TRY(m_volume->inode(number));
    auto* raw = new (Base::nothrow) VultureFSInode(*this, number, record);
    if (!raw)
        return Error::from_errno(ENOMEM);
    auto inode = adopt_ref(*raw);
    TRY(m_inodes.try_set(number, RefPtr<VultureFSInode>(inode)));
    return inode;
}

void VultureFileSystem::refresh(InodeNumber number)
{
    auto* cached = m_inodes.get(number);
    if (!cached)
        return;
    if (auto record = m_volume->inode(number); !record.is_error())
        (*cached)->set_record(record.value());
}

ErrorOr<void> VultureFileSystem::unlinked(VultureFS::Unlinked unlinked)
{
    if (m_inodes.get(unlinked.inode)) {
        // Still cached, maybe open: deleted when forgotten.
        refresh(unlinked.inode);
        return {};
    }
    if (unlinked.last_link)
        TRY(m_volume->release(unlinked.inode));
    return {};
}

ErrorOr<void> VultureFileSystem::sync()
{
    MutexLocker locker(m_lock);
    return sync_locked();
}

ErrorOr<void> VultureFileSystem::sync_locked()
{
    Vector<NonnullRefPtr<VultureFSInode>> dirty;
    m_inodes.for_each([&](InodeNumber, RefPtr<VultureFSInode>& inode) {
        if (inode->is_dirty())
            dirty.append(*inode);
    });
    for (auto& inode : dirty)
        TRY(inode->write_back());
    dirty.clear();
    forget_unused_inodes();
    return m_volume->checkpoint();
}

// Drops inodes nobody else holds: always those with no names left (which
// deletes them), the rest when the cache is large.
void VultureFileSystem::forget_unused_inodes()
{
    bool over_budget = m_cached_pages > cached_page_limit || m_inodes.size() > cached_inode_limit;
    Vector<InodeNumber> forget;
    m_inodes.for_each([&](InodeNumber number, RefPtr<VultureFSInode>& inode) {
        if (inode->ref_count() != 1 || inode->is_dirty() || number == VultureFS::root_directory)
            return;
        if (inode->record().link_count == 0 || over_budget)
            forget.append(number);
    });
    for (auto number : forget) {
        bool deleted = (*m_inodes.get(number))->record().link_count == 0;
        m_inodes.remove(number);
        if (deleted) {
            if (auto result = m_volume->release(number); result.is_error())
                dmesgln("vultureFS: cannot delete inode {}: {}", number, result.error());
        }
    }
}

ErrorOr<void> VultureFileSystem::rename(Inode& old_parent, StringView old_name, Inode& new_parent, StringView new_name)
{
    MutexLocker locker(m_lock);
    auto moved = TRY(m_volume->lookup(old_parent.index(), old_name));
    auto replaced = TRY(m_volume->rename(old_parent.index(), old_name, new_parent.index(), new_name));
    if (replaced.has_value())
        TRY(unlinked(replaced.value()));
    refresh(old_parent.index());
    refresh(new_parent.index());
    refresh(moved.inode);
    return {};
}

InodeMetadata VultureFSInode::metadata_locked() const
{
    InodeMetadata metadata;
    metadata.index = index();
    metadata.type = type_of(m_record);
    metadata.permissions = m_record.mode & 07777;
    metadata.link_count = m_record.link_count;
    metadata.uid = m_record.uid;
    metadata.gid = m_record.gid;
    metadata.size = m_record.size;
    metadata.modification_time = m_record.modification_time / 1'000'000'000;
    metadata.access_time_ns = m_record.access_time;
    metadata.modification_time_ns = m_record.modification_time;
    metadata.change_time_ns = m_record.change_time;
    metadata.birth_time_ns = m_record.birth_time;
    metadata.block_size = VultureFS::block_size;
    return metadata;
}

ErrorOr<void> VultureFSInode::read_ahead(u64 index)
{
    size_t count = 0;
    while (count < read_ahead_pages && (index + count) * page_size < m_record.size && !m_pages.get(index + count))
        ++count;
    if (count == 0)
        count = 1;
    auto* buffer = static_cast<u8*>(kmalloc(count * page_size));
    if (!buffer)
        return Error::from_errno(ENOMEM);
    auto read = fs().volume().read(this->index(), index * page_size, { buffer, count * page_size });
    if (read.is_error()) {
        kfree(buffer);
        return read.release_error();
    }
    size_t got = read.value();
    ErrorOr<void> result {};
    for (size_t i = 0; i < count && !result.is_error(); ++i) {
        auto page = physical_memory().allocate_user_page();
        if (page.is_error()) {
            result = page.release_error();
            break;
        }
        if (i * page_size < got)
            __builtin_memcpy(physical_to_virtual(page.value()), buffer + i * page_size, min(page_size, got - i * page_size));
        result = m_pages.try_set(index + i, CachedPage { page.value(), false });
        if (result.is_error())
            physical_memory().unref_user_page(page.value());
        else
            fs().count_pages(1, 0);
    }
    kfree(buffer);
    return result;
}

ErrorOr<PhysicalAddress> VultureFSInode::page(u64 index, bool load)
{
    if (!m_pages.get(index)) {
        if (load) {
            TRY(read_ahead(index));
        } else {
            auto page = TRY(physical_memory().allocate_user_page());
            if (auto result = m_pages.try_set(index, CachedPage { page, false }); result.is_error()) {
                physical_memory().unref_user_page(page);
                return result.release_error();
            }
            fs().count_pages(1, 0);
        }
    }
    auto address = m_pages.get(index)->address;
    physical_memory().ref_user_page(address);
    return address;
}

void VultureFSInode::mark_dirty(u64 index, PhysicalAddress address)
{
    auto* cached = m_pages.get(index);
    // Gone (the file shrank meanwhile): the write is lost with it.
    if (!cached || cached->address.get() != address.get() || cached->dirty)
        return;
    cached->dirty = true;
    ++m_dirty_count;
    fs().count_pages(0, 1);
}

void VultureFSInode::drop_pages_from(u64 first)
{
    Vector<u64> dropped;
    m_pages.for_each([&](u64 index, CachedPage&) {
        if (index >= first)
            dropped.append(index);
    });
    for (auto index : dropped) {
        auto page = *m_pages.get(index);
        if (page.dirty) {
            --m_dirty_count;
            fs().count_pages(0, -1);
        }
        physical_memory().unref_user_page(page.address);
        m_pages.remove(index);
        fs().count_pages(-1, 0);
    }
}

ErrorOr<size_t> VultureFSInode::read(u64 offset, size_t count, IOBuffer const& buffer)
{
    size_t done = 0;
    while (done < count) {
        PhysicalAddress address;
        size_t in_page = (offset + done) % page_size;
        size_t chunk;
        {
            MutexLocker locker(fs().lock());
            auto type = type_of(m_record);
            if (type != InodeType::File)
                return Error::from_errno(type == InodeType::Directory ? EISDIR : EINVAL);
            if (offset + done >= m_record.size)
                break;
            chunk = min<size_t>(min<size_t>(count - done, page_size - in_page), m_record.size - (offset + done));
            address = TRY(page((offset + done) / page_size, true));
        }
        // Copied without the lock (it may fault); the reference keeps the
        // page alive even if the file shrinks meanwhile.
        auto result = buffer.write(done, { physical_to_virtual(address) + in_page, chunk });
        physical_memory().unref_user_page(address);
        TRY(result);
        done += chunk;
    }
    return done;
}

ErrorOr<size_t> VultureFSInode::write(u64 offset, size_t count, IOBuffer const& buffer)
{
    size_t done = 0;
    while (done < count) {
        size_t in_page = (offset + done) % page_size;
        size_t chunk = min(count - done, page_size - in_page);
        u64 index = (offset + done) / page_size;
        PhysicalAddress address;
        {
            MutexLocker locker(fs().lock());
            auto type = type_of(m_record);
            if (type != InodeType::File)
                return Error::from_errno(type == InodeType::Directory ? EISDIR : EINVAL);
            // What is there already matters unless all of the page is
            // overwritten or it lies past the end.
            bool whole = in_page == 0 && chunk == page_size;
            bool past_end = index * page_size >= m_record.size;
            address = TRY(page(index, !whole && !past_end));
        }
        auto result = buffer.read(done, { physical_to_virtual(address) + in_page, chunk });
        bool sync_now = false;
        {
            MutexLocker locker(fs().lock());
            if (!result.is_error()) {
                // Dirty only now that the bytes are in: a write-back in
                // between must not take the page as written.
                mark_dirty(index, address);
                m_record.size = max<u64>(m_record.size, offset + done + chunk);
                m_record.modification_time = kernel_clock();
                m_times_dirty = true;
            }
            sync_now = fs().has_too_many_dirty_pages();
        }
        physical_memory().unref_user_page(address);
        TRY(result);
        done += chunk;
        if (sync_now)
            TRY(fs().sync());
    }
    return done;
}

ErrorOr<void> VultureFSInode::truncate(u64 size)
{
    MutexLocker locker(fs().lock());
    auto type = type_of(m_record);
    if (type != InodeType::File)
        return Error::from_errno(type == InodeType::Directory ? EISDIR : EINVAL);
    if (size < m_record.size) {
        drop_pages_from((size + page_size - 1) / page_size);
        // Bytes past the end read as zeros if the file grows again.
        if (size % page_size) {
            if (auto* cached = m_pages.get(size / page_size))
                __builtin_memset(physical_to_virtual(cached->address) + size % page_size, 0, page_size - size % page_size);
        }
    }
    TRY(fs().volume().truncate(index(), size));
    auto record = TRY(fs().volume().inode(index()));
    m_record = record;
    m_record.size = size;
    return {};
}

ErrorOr<PhysicalAddress> VultureFSInode::mapped_page(u64 index)
{
    MutexLocker locker(fs().lock());
    if (type_of(m_record) != InodeType::File)
        return Error::from_errno(ENODEV);
    return page(index, true);
}

void VultureFSInode::add_shared_writer()
{
    MutexLocker locker(fs().lock());
    ++m_shared_writers;
}

void VultureFSInode::mark_all_dirty()
{
    Vector<u64> indices;
    m_pages.for_each([&](u64 index, CachedPage& page) {
        if (!page.dirty)
            indices.append(index);
    });
    for (auto index : indices)
        mark_dirty(index, m_pages.get(index)->address);
}

void VultureFSInode::remove_shared_writer()
{
    MutexLocker locker(fs().lock());
    VERIFY(m_shared_writers > 0);
    // What the mapping wrote since the last write-back is still to go.
    if (--m_shared_writers == 0)
        mark_all_dirty();
}

ErrorOr<void> VultureFSInode::write_back()
{
    if (m_shared_writers > 0)
        mark_all_dirty();
    if (m_dirty_count > 0) {
        Vector<u64> dirty;
        m_pages.for_each([&](u64 index, CachedPage& page) {
            if (page.dirty)
                dirty.append(index);
        });
        quick_sort(Span<u64>(dirty.begin(), dirty.size()));
        auto* buffer = static_cast<u8*>(kmalloc(write_back_pages * page_size));
        if (!buffer)
            return Error::from_errno(ENOMEM);
        ErrorOr<void> result {};
        for (size_t first = 0; first < dirty.size() && !result.is_error();) {
            // A run of consecutive pages, written in one go.
            size_t run = 1;
            while (first + run < dirty.size() && run < write_back_pages && dirty[first + run] == dirty[first] + run)
                ++run;
            u64 start = dirty[first] * page_size;
            if (start >= m_record.size) {
                first += run;
                continue;
            }
            for (size_t i = 0; i < run; ++i)
                __builtin_memcpy(buffer + i * page_size, physical_to_virtual(m_pages.get(dirty[first + i])->address), page_size);
            size_t length = static_cast<size_t>(min<u64>(run * page_size, m_record.size - start));
            result = fs().volume().write(index(), start, { buffer, length });
            if (!result.is_error()) {
                for (size_t i = 0; i < run; ++i) {
                    m_pages.get(dirty[first + i])->dirty = false;
                    --m_dirty_count;
                    fs().count_pages(0, -1);
                }
            }
            first += run;
        }
        kfree(buffer);
        TRY(result);
        // Pages past the end were dropped on truncate; any left are stale.
        for (auto index : dirty) {
            auto* cached = m_pages.get(index);
            if (cached && cached->dirty) {
                cached->dirty = false;
                --m_dirty_count;
                fs().count_pages(0, -1);
            }
        }
    }
    if (m_times_dirty) {
        TRY(fs().volume().set_times(index(), m_record.access_time, m_record.modification_time));
        m_times_dirty = false;
    }
    return {};
}

ErrorOr<NonnullRefPtr<Inode>> VultureFSInode::lookup(StringView name)
{
    MutexLocker locker(fs().lock());
    if (type_of(m_record) != InodeType::Directory)
        return Error::from_errno(ENOTDIR);
    auto entry = TRY(fs().volume().lookup(index(), name));
    return NonnullRefPtr<Inode>(*TRY(fs().inode(entry.inode)));
}

ErrorOr<void> VultureFSInode::traverse(DirectoryVisitor& visitor)
{
    struct Entry {
        String name;
        InodeIndex index;
        InodeType type;
    };
    Vector<Entry> entries;
    {
        MutexLocker locker(fs().lock());
        if (type_of(m_record) != InodeType::Directory)
            return Error::from_errno(ENOTDIR);
        TRY(entries.try_append(Entry { TRY(String::from_view("."_sv)), index(), InodeType::Directory }));
        TRY(entries.try_append(Entry { TRY(String::from_view(".."_sv)), m_record.parent, InodeType::Directory }));
        ErrorOr<void> result {};
        TRY(fs().volume().for_each_entry(index(), [&](StringView name, VultureFS::DirectoryEntry entry) {
            auto copy = String::from_view(name);
            if (copy.is_error()) {
                result = copy.release_error();
                return VultureFS::Iteration::Stop;
            }
            auto type = entry.type == VultureFS::type_directory ? InodeType::Directory
                : entry.type == VultureFS::type_symbolic_link   ? InodeType::SymbolicLink
                                                                : InodeType::File;
            result = entries.try_append(Entry { copy.release_value(), entry.inode, type });
            return result.is_error() ? VultureFS::Iteration::Stop : VultureFS::Iteration::Continue;
        }));
        TRY(result);
    }
    for (auto const& entry : entries) {
        if (visitor.visit(entry.name.view(), entry.index, entry.type) == VisitDecision::Stop)
            break;
    }
    return {};
}

ErrorOr<NonnullRefPtr<Inode>> VultureFSInode::create_child(StringView name, InodeType type, u32 permissions, StringView link_target)
{
    u32 mode;
    switch (type) {
    case InodeType::File:
        mode = VultureFS::mode_regular;
        break;
    case InodeType::Directory:
        mode = VultureFS::mode_directory;
        break;
    case InodeType::SymbolicLink:
        mode = VultureFS::mode_symbolic_link;
        break;
    default:
        return Error::from_errno(ENOTSUP);
    }
    MutexLocker locker(fs().lock());
    if (type_of(m_record) != InodeType::Directory)
        return Error::from_errno(ENOTDIR);
    auto number = TRY(fs().volume().create(index(), name, mode | (permissions & 07777), 0, 0, link_target));
    fs().refresh(index());
    return NonnullRefPtr<Inode>(*TRY(fs().inode(number)));
}

ErrorOr<void> VultureFSInode::remove_child(StringView name)
{
    MutexLocker locker(fs().lock());
    if (type_of(m_record) != InodeType::Directory)
        return Error::from_errno(ENOTDIR);
    auto entry = TRY(fs().volume().lookup(index(), name));
    auto unlinked = entry.type == VultureFS::type_directory ? TRY(fs().volume().remove_directory(index(), name))
                                                            : TRY(fs().volume().unlink(index(), name));
    TRY(fs().unlinked(unlinked));
    fs().refresh(index());
    return {};
}

ErrorOr<void> VultureFSInode::add_child(StringView name, Inode& inode)
{
    MutexLocker locker(fs().lock());
    TRY(fs().volume().link(index(), name, inode.index()));
    fs().refresh(index());
    fs().refresh(inode.index());
    return {};
}

ErrorOr<size_t> VultureFSInode::read_link(Bytes buffer)
{
    MutexLocker locker(fs().lock());
    return fs().volume().read_link(index(), buffer);
}

ErrorOr<void> VultureFSInode::set_permissions(u32 permissions)
{
    MutexLocker locker(fs().lock());
    TRY(fs().volume().set_permissions(index(), permissions));
    fs().refresh(index());
    return {};
}

ErrorOr<void> VultureFSInode::set_owner(u32 uid, u32 gid)
{
    MutexLocker locker(fs().lock());
    TRY(fs().volume().set_owner(index(), uid, gid));
    fs().refresh(index());
    return {};
}

ErrorOr<void> VultureFSInode::set_times(i64 access_time, i64 modification_time)
{
    MutexLocker locker(fs().lock());
    m_record.access_time = access_time;
    m_record.modification_time = modification_time;
    TRY(fs().volume().set_times(index(), access_time, modification_time));
    m_times_dirty = false;
    fs().refresh(index());
    return {};
}

bool is_vulturefs_partition(BlockDevice& device)
{
    auto* partition = device.as_partition();
    return partition && __builtin_memcmp(partition->type(), VultureFS::partition_type, 16) == 0;
}

ErrorOr<NonnullRefPtr<FileSystem>> mount_vulturefs(BlockDevice& device)
{
    auto file_system = TRY(VultureFileSystem::create(device));
    auto statistics = file_system->volume().statistics();
    dmesgln("vultureFS: {} \"{}\": checkpoint {}, {} of {} MiB free", device.name(), file_system->volume().name(), statistics.generation,
        statistics.free_blocks * VultureFS::block_size / MiB, statistics.block_count * VultureFS::block_size / MiB);
    return NonnullRefPtr<FileSystem>(*file_system);
}

}
