/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/files.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/ForeignFS.h>
#include <Kernel/FileSystem/Inode.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Time/Time.h>
#include <LibBase/HashMap.h>
#include <LibBase/New.h>

namespace Kernel {

namespace {

i64 kernel_seconds()
{
    return static_cast<i64>(Time::wall_clock_nanoseconds() / 1'000'000'000);
}

class DeviceStore final : public ForeignFS::Store {
public:
    explicit DeviceStore(BlockDevice& device)
        : m_device(device)
    {
    }

    [[nodiscard]] u64 sector_count() const override { return m_device->block_count(); }
    [[nodiscard]] bool is_read_only() const override { return m_device->is_read_only(); }
    ErrorOr<void> read_sectors(u64 first, size_t count, Bytes buffer) override { return m_device->read_blocks(first, count, buffer); }
    ErrorOr<void> write_sectors(u64 first, size_t count, ReadonlyBytes buffer) override { return m_device->write_blocks(first, count, buffer); }
    ErrorOr<void> flush() override { return m_device->flush(); }

    [[nodiscard]] BlockDevice& device() { return *m_device; }
    [[nodiscard]] bool has_gone() const { return m_device->has_gone(); }

private:
    NonnullRefPtr<BlockDevice> m_device;
};

class ForeignInode;

class ForeignFileSystem final : public FileSystem {
public:
    static ErrorOr<NonnullRefPtr<ForeignFileSystem>> create(BlockDevice& device, bool read_only)
    {
        auto* raw = new (Base::nothrow) ForeignFileSystem(device);
        if (!raw)
            return Error::from_errno(ENOMEM);
        auto file_system = adopt_ref(*raw);
        file_system->m_device_name = TRY(String::from_view(device.name()));
        file_system->m_volume = TRY(ForeignFS::open_volume(file_system->m_store, kernel_seconds, read_only));
        file_system->m_type = TRY(String::from_view(file_system->m_volume->type_name()));
        return file_system;
    }

    ~ForeignFileSystem() override;

    [[nodiscard]] StringView name() const override { return m_type.view(); }
    [[nodiscard]] NonnullRefPtr<Inode> root_inode() override;
    [[nodiscard]] bool is_read_only() const override { return m_volume->is_read_only(); }
    [[nodiscard]] StringView device_name() const override { return m_device_name.view(); }
    [[nodiscard]] bool device_has_gone() const override { return m_store.has_gone(); }
    [[nodiscard]] Statistics statistics() override
    {
        MutexLocker locker(m_lock);
        auto statistics = m_volume->statistics();
        if (statistics.is_error())
            return {};
        return { statistics.value().block_size, statistics.value().block_count, statistics.value().free_blocks };
    }
    ErrorOr<void> sync() override;
    ErrorOr<void> rename(Inode& old_parent, StringView old_name, Inode& new_parent, StringView new_name) override;

    [[nodiscard]] Mutex& lock() { return m_lock; }
    [[nodiscard]] ForeignFS::Volume& volume() { return *m_volume; }
    [[nodiscard]] BlockDevice& device() { return m_store.device(); }
    ErrorOr<NonnullRefPtr<ForeignInode>> inode_for(ForeignFS::NodeInfo const&);
    void removed(ForeignFS::NodeIndex);
    void gone(ForeignFS::NodeIndex);
    void let_go_of_retired();
    ErrorOr<void> write_back_pages();

private:
    explicit ForeignFileSystem(BlockDevice& device)
        : m_store(device)
    {
    }

    DeviceStore m_store;
    String m_device_name;
    String m_type;
    OwnPtr<ForeignFS::Volume> m_volume;
    Mutex m_lock;
    // Those it's known about since it was mounted: one each, for things to be the same when they're found again.
    HashMap<ForeignFS::NodeIndex, RefPtr<ForeignInode>> m_inodes;
    Vector<RefPtr<ForeignInode>> m_retiring;
    // All that exist, whoever's holding them: they may be held for a moment longer than this lasts.
    HashMap<ForeignFS::NodeIndex, ForeignInode*> m_alive;
};

class ForeignInode final : public Inode {
public:
    ForeignInode(ForeignFileSystem& file_system, ForeignFS::NodeInfo const& info)
        : Inode(file_system, info.index)
        , m_info(info)
    {
    }

    ~ForeignInode() override
    {
        for_each_page([](PhysicalAddress page) { physical_memory().unref_user_page(page); });
        if (!m_detached)
            fs().gone(index());
    }

    // What it was on has gone, and it has nothing to tell it.
    void detach() { m_detached = true; }

    [[nodiscard]] InodeMetadata metadata() const override
    {
        MutexLocker locker(fs().lock());
        auto const refreshed = fs().volume().info(index());
        auto const& info = refreshed.is_error() ? m_info : refreshed.value();
        InodeMetadata metadata;
        metadata.index = index();
        metadata.type = info.is_directory ? InodeType::Directory : InodeType::File;
        // These have no owners to speak of: so it's anyone's, as it is on a Mac.
        metadata.permissions = info.read_only ? 0555 : 0777;
        metadata.link_count = info.is_directory ? 2 : 1;
        metadata.size = info.is_directory ? 0 : info.size;
        metadata.modification_time = info.modified;
        metadata.modification_time_ns = info.modified * 1'000'000'000;
        metadata.access_time_ns = info.accessed * 1'000'000'000;
        metadata.allocated_bytes = info.allocated;
        metadata.flags = info.hidden ? UF_HIDDEN : 0;
        metadata.block_size = 4096;
        return metadata;
    }

    ErrorOr<ContiguousExtent> contiguous_extent() const override
    {
        MutexLocker locker(fs().lock());
        auto const extent = TRY(fs().volume().extent(index()));
        return ContiguousExtent { &fs().device(), extent.first_sector, extent.size };
    }

    ErrorOr<size_t> read(u64 offset, size_t count, IOBuffer const& buffer) override
    {
        constexpr size_t chunk_size = 64 * KiB;
        auto* chunk = static_cast<u8*>(kmalloc(min(count, chunk_size)));
        if (!chunk)
            return Error::from_errno(ENOMEM);
        size_t done = 0;
        ErrorOr<void> result = {};
        while (done < count) {
            size_t const part = min(count - done, chunk_size);
            ErrorOr<size_t> got = 0;
            {
                MutexLocker locker(fs().lock());
                got = fs().volume().read(index(), offset + done, { chunk, part });
            }
            if (got.is_error()) {
                result = got.release_error();
                break;
            }
            if (got.value() == 0)
                break;
            auto copied = buffer.write(done, { chunk, got.value() });
            if (copied.is_error()) {
                result = copied.release_error();
                break;
            }
            done += got.value();
            if (got.value() < part)
                break;
        }
        kfree(chunk);
        TRY(result);
        return done;
    }

    ErrorOr<size_t> write(u64 offset, size_t count, IOBuffer const& buffer) override
    {
        constexpr size_t chunk_size = 64 * KiB;
        auto* chunk = static_cast<u8*>(kmalloc(min(count, chunk_size)));
        if (!chunk)
            return Error::from_errno(ENOMEM);
        size_t done = 0;
        ErrorOr<void> result = {};
        while (done < count) {
            size_t const part = min(count - done, chunk_size);
            auto copied = buffer.read(done, { chunk, part });
            if (copied.is_error()) {
                result = copied.release_error();
                break;
            }
            ErrorOr<size_t> wrote = 0;
            {
                MutexLocker locker(fs().lock());
                wrote = fs().volume().write(index(), offset + done, { chunk, part });
                if (!wrote.is_error())
                    update_mapped_pages(offset + done, { chunk, wrote.value() });
            }
            if (wrote.is_error()) {
                result = wrote.release_error();
                break;
            }
            done += wrote.value();
        }
        kfree(chunk);
        TRY(result);
        return done;
    }

    ErrorOr<void> truncate(u64 size) override
    {
        MutexLocker locker(fs().lock());
        TRY(fs().volume().truncate(index(), size));
        drop_pages_from((size + page_size - 1) / page_size);
        return {};
    }

    ErrorOr<NonnullRefPtr<Inode>> lookup(StringView name) override
    {
        MutexLocker locker(fs().lock());
        auto const info = TRY(fs().volume().lookup(index(), name));
        return NonnullRefPtr<Inode>(*TRY(fs().inode_for(info)));
    }

    ErrorOr<void> traverse(DirectoryVisitor& visitor) override
    {
        MutexLocker locker(fs().lock());
        if (visitor.visit("."_sv, index(), InodeType::Directory) == VisitDecision::Stop)
            return {};
        auto const parent = TRY(fs().volume().lookup(index(), ".."_sv));
        if (visitor.visit(".."_sv, parent.index, InodeType::Directory) == VisitDecision::Stop)
            return {};
        return fs().volume().for_each_entry(index(), [&](StringView name, ForeignFS::NodeInfo const& info) {
            auto const decision = visitor.visit(name, info.index, info.is_directory ? InodeType::Directory : InodeType::File);
            return decision == VisitDecision::Stop ? ForeignFS::Iteration::Stop : ForeignFS::Iteration::Continue;
        });
    }

    ErrorOr<NonnullRefPtr<Inode>> create_child(StringView name, InodeType type, u32, StringView) override
    {
        if (type != InodeType::File && type != InodeType::Directory)
            return Error::from_errno(ENOTSUP);
        MutexLocker locker(fs().lock());
        auto const info = TRY(fs().volume().create(index(), name, type == InodeType::Directory));
        return NonnullRefPtr<Inode>(*TRY(fs().inode_for(info)));
    }

    ErrorOr<void> remove_child(StringView name) override
    {
        {
            MutexLocker locker(fs().lock());
            auto const info = TRY(fs().volume().lookup(index(), name));
            TRY(fs().volume().remove(index(), name));
            fs().removed(info.index);
        }
        fs().let_go_of_retired();
        return {};
    }

    ErrorOr<void> add_child(StringView, Inode&) override { return Error::from_errno(ENOTSUP); }

    // There are no owners, and the only thing a mode can say is whether it can be written.
    ErrorOr<void> set_permissions(u32 permissions) override
    {
        MutexLocker locker(fs().lock());
        if (fs().volume().is_read_only())
            return Error::from_errno(EROFS);
        return fs().volume().set_read_only(index(), !(permissions & 0222));
    }

    ErrorOr<void> set_owner(u32, u32) override { return {}; }

    ErrorOr<void> set_times(i64 access_time, i64 modification_time) override
    {
        MutexLocker locker(fs().lock());
        return fs().volume().set_times(index(), access_time / 1'000'000'000, modification_time / 1'000'000'000);
    }

    ErrorOr<void> sync() override { return fs().sync(); }

    ErrorOr<PhysicalAddress> mapped_page(u64 page) override
    {
        MutexLocker locker(fs().lock());
        if (auto const* cached = m_pages.get(page)) {
            physical_memory().ref_user_page(*cached);
            return *cached;
        }
        auto const address = TRY(physical_memory().allocate_user_page());
        __builtin_memset(physical_to_virtual(address), 0, page_size);
        auto got = fs().volume().read(index(), page * page_size, { physical_to_virtual(address), page_size });
        if (got.is_error()) {
            physical_memory().unref_user_page(address);
            return got.release_error();
        }
        TRY(m_pages.try_set(page, address));
        physical_memory().ref_user_page(address);
        return address;
    }

    void add_shared_writer() override { ++m_shared_writers; }
    void remove_shared_writer() override
    {
        // What was changed that way goes where it belongs now.
        (void)write_back_pages();
        --m_shared_writers;
    }

    ErrorOr<void> write_back_pages()
    {
        if (m_shared_writers == 0)
            return {};
        ErrorOr<void> result = {};
        m_pages.for_each([&](u64 page, PhysicalAddress address) {
            auto const size = fs().volume().info(index());
            if (size.is_error() || page * page_size >= size.value().size)
                return;
            auto const part = min<u64>(page_size, size.value().size - page * page_size);
            auto const wrote = fs().volume().write(index(), page * page_size, { physical_to_virtual(address), static_cast<size_t>(part) });
            if (wrote.is_error() && !result.is_error())
                result = wrote.error();
        });
        return result;
    }

private:
    [[nodiscard]] ForeignFileSystem& fs() const { return static_cast<ForeignFileSystem&>(file_system()); }

    template<typename Callback>
    void for_each_page(Callback callback)
    {
        m_pages.for_each([&](u64, PhysicalAddress address) { callback(address); });
    }

    void update_mapped_pages(u64 offset, ReadonlyBytes bytes)
    {
        for (u64 page = offset / page_size; page * page_size < offset + bytes.size(); ++page) {
            auto const* cached = m_pages.get(page);
            if (!cached)
                continue;
            u64 const start = max(page * page_size, offset);
            u64 const end = min((page + 1) * page_size, offset + bytes.size());
            __builtin_memcpy(physical_to_virtual(*cached) + start - page * page_size, bytes.data() + (start - offset), end - start);
        }
    }

    void drop_pages_from(u64 first)
    {
        Vector<u64> dropping;
        m_pages.for_each([&](u64 page, PhysicalAddress address) {
            if (page >= first) {
                dropping.append(page);
                physical_memory().unref_user_page(address);
            }
        });
        for (auto page : dropping)
            m_pages.remove(page);
    }

    ForeignFS::NodeInfo m_info;
    HashMap<u64, PhysicalAddress> m_pages;
    size_t m_shared_writers { 0 };
    bool m_detached { false };
};

ForeignFileSystem::~ForeignFileSystem()
{
    m_alive.for_each([](ForeignFS::NodeIndex, ForeignInode*& inode) { inode->detach(); });
}

NonnullRefPtr<Inode> ForeignFileSystem::root_inode()
{
    MutexLocker locker(m_lock);
    auto const info = MUST(m_volume->info(m_volume->root()));
    return NonnullRefPtr<Inode>(*MUST(inode_for(info)));
}

ErrorOr<NonnullRefPtr<ForeignInode>> ForeignFileSystem::inode_for(ForeignFS::NodeInfo const& info)
{
    if (auto const* known = m_inodes.get(info.index))
        return NonnullRefPtr<ForeignInode>(**known);
    auto* raw = new (Base::nothrow) ForeignInode(*this, info);
    if (!raw)
        return Error::from_errno(ENOMEM);
    auto inode = adopt_ref(*raw);
    TRY(m_alive.try_set(info.index, raw));
    TRY(m_inodes.try_set(info.index, RefPtr<ForeignInode>(inode.ptr())));
    return inode;
}

void ForeignFileSystem::removed(ForeignFS::NodeIndex index)
{
    auto* known = m_inodes.get(index);
    if (!known) {
        // Nothing's using it, so there's nothing to wait for.
        m_volume->forget(index);
        return;
    }
    // Nobody can find it any more: it goes with whatever's still holding it. And if that's nothing, it goes the next time
    // this is here, as the last of it wants the lock that's held now.
    (void)m_retiring.try_append(move(*known));
    m_inodes.remove(index);
}

void ForeignFileSystem::let_go_of_retired()
{
    Vector<RefPtr<ForeignInode>> going;
    {
        MutexLocker locker(m_lock);
        going = move(m_retiring);
        m_retiring = {};
    }
}

void ForeignFileSystem::gone(ForeignFS::NodeIndex index)
{
    MutexLocker locker(m_lock);
    m_alive.remove(index);
    m_volume->forget(index);
}

ErrorOr<void> ForeignFileSystem::sync()
{
    MutexLocker locker(m_lock);
    if (m_volume->is_read_only())
        return {};
    ErrorOr<void> result = {};
    m_inodes.for_each([&](ForeignFS::NodeIndex, RefPtr<ForeignInode>& inode) {
        auto const wrote = inode->write_back_pages();
        if (wrote.is_error() && !result.is_error())
            result = wrote.error();
    });
    TRY(result);
    return m_volume->flush();
}

ErrorOr<void> ForeignFileSystem::rename(Inode& old_parent, StringView old_name, Inode& new_parent, StringView new_name)
{
    {
        MutexLocker locker(m_lock);
        auto const moving = TRY(m_volume->lookup(old_parent.index(), old_name));
        auto const replaced = m_volume->lookup(new_parent.index(), new_name);
        TRY(m_volume->rename(old_parent.index(), old_name, new_parent.index(), new_name));
        if (!replaced.is_error() && replaced.value().index != moving.index)
            removed(replaced.value().index);
    }
    let_go_of_retired();
    return {};
}

}

ForeignFS::Kind recognise_file_system(BlockDevice& device)
{
    DeviceStore store(device);
    return ForeignFS::recognise(store);
}

ErrorOr<String> label_of_file_system(BlockDevice& device)
{
    DeviceStore store(device);
    auto volume = TRY(ForeignFS::open_volume(store, kernel_seconds, true));
    return String::from_view(volume->label());
}

ErrorOr<NonnullRefPtr<FileSystem>> mount_foreign_file_system(BlockDevice& device, bool read_only)
{
    return NonnullRefPtr<FileSystem>(*TRY(ForeignFileSystem::create(device, read_only)));
}

}
