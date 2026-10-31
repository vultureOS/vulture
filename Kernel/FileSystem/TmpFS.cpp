/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/TmpFS.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Time/Time.h>
#include <LibBase/New.h>

namespace Kernel {

ErrorOr<NonnullRefPtr<TmpFS>> TmpFS::create()
{
    auto* file_system = new (Base::nothrow) TmpFS;
    if (!file_system)
        return Error::from_errno(ENOMEM);
    auto reference = adopt_ref(*file_system);
    reference->m_root = TRY(TmpFSInode::create(*reference, InodeType::Directory, 0755));
    reference->m_root->m_metadata.index = 1;
    return reference;
}

NonnullRefPtr<Inode> TmpFS::root_inode()
{
    return *m_root;
}

ErrorOr<void> TmpFS::rename(Inode& old_parent_inode, StringView old_name, Inode& new_parent_inode, StringView new_name)
{
    auto& old_parent = static_cast<TmpFSInode&>(old_parent_inode);
    auto& new_parent = static_cast<TmpFSInode&>(new_parent_inode);
    MutexLocker locker(m_lock);
    auto* moving = old_parent.find_child(old_name);
    if (!moving)
        return Error::from_errno(ENOENT);
    auto inode = moving->inode;
    if (auto* existing = new_parent.find_child(new_name)) {
        if (existing->inode.ptr() == inode.ptr())
            return {};
        if (existing->inode->m_metadata.type == InodeType::Directory && !existing->inode->m_children.is_empty())
            return Error::from_errno(ENOTEMPTY);
        if (inode->m_metadata.type == InodeType::Directory && existing->inode->m_metadata.type != InodeType::Directory)
            return Error::from_errno(ENOTDIR);
        if (inode->m_metadata.type != InodeType::Directory && existing->inode->m_metadata.type == InodeType::Directory)
            return Error::from_errno(EISDIR);
        existing->inode = inode;
    } else {
        TRY(new_parent.m_children.try_append(TmpFSInode::Child { TRY(String::from_view(new_name)), inode }));
    }
    old_parent.m_children.remove_all_matching([&](auto const& child) { return child.name == old_name && child.inode.ptr() == inode.ptr(); });
    inode->m_parent = &new_parent;
    return {};
}

TmpFSInode::TmpFSInode(TmpFS& file_system, InodeIndex index, InodeType type, u32 permissions)
    : Inode(file_system, index)
{
    m_metadata.index = index;
    m_metadata.type = type;
    m_metadata.permissions = permissions;
    m_metadata.link_count = type == InodeType::Directory ? 2 : 1;
    m_metadata.modification_time = Time::wall_clock_seconds();
}

ErrorOr<NonnullRefPtr<TmpFSInode>> TmpFSInode::create(TmpFS& file_system, InodeType type, u32 permissions)
{
    auto* inode = new (Base::nothrow) TmpFSInode(file_system, file_system.allocate_index(), type, permissions);
    if (!inode)
        return Error::from_errno(ENOMEM);
    return adopt_ref(*inode);
}

TmpFSInode::~TmpFSInode()
{
    free_pages_from(0);
}

InodeMetadata TmpFSInode::metadata() const
{
    return m_metadata;
}

void TmpFSInode::free_pages_from(size_t first_page)
{
    for (size_t i = first_page; i < m_pages.size(); ++i) {
        if (m_pages[i].get())
            physical_memory().unref_user_page(m_pages[i]);
    }
    if (first_page < m_pages.size())
        MUST(m_pages.try_resize(first_page));
}

ErrorOr<PhysicalAddress> TmpFSInode::page_at(u64 offset, bool allocate)
{
    size_t index = offset / page_size;
    if (index >= m_pages.size()) {
        if (!allocate)
            return PhysicalAddress();
        TRY(m_pages.try_resize(index + 1));
    }
    if (!m_pages[index].get() && allocate)
        m_pages[index] = TRY(physical_memory().allocate_user_page());
    if (m_pages[index].get())
        physical_memory().ref_user_page(m_pages[index]);
    return m_pages[index];
}

ErrorOr<PhysicalAddress> TmpFSInode::mapped_page(u64 index)
{
    if (m_metadata.type != InodeType::File)
        return Error::from_errno(ENODEV);
    MutexLocker locker(tmpfs().lock());
    // tmpfs pages are the file: a mapping shares them as they are.
    return page_at(index * page_size, true);
}

ErrorOr<size_t> TmpFSInode::read(u64 offset, size_t count, IOBuffer const& buffer)
{
    if (m_metadata.type == InodeType::Directory)
        return Error::from_errno(EISDIR);
    size_t done = 0;
    while (done < count) {
        PhysicalAddress page;
        size_t chunk;
        {
            MutexLocker locker(tmpfs().lock());
            if (offset + done >= m_metadata.size)
                break;
            size_t in_page = (offset + done) % page_size;
            chunk = min<size_t>(min<size_t>(count - done, page_size - in_page), m_metadata.size - (offset + done));
            page = TRY(page_at(offset + done, false));
        }
        size_t in_page = (offset + done) % page_size;
        // Copied without the lock (it may fault); the reference keeps the
        // page alive even if the file shrinks meanwhile.
        auto result = page.get() ? buffer.write(done, { physical_to_virtual(page) + in_page, chunk }) : buffer.fill_with_zeros(done, chunk);
        if (page.get())
            physical_memory().unref_user_page(page);
        TRY(result);
        done += chunk;
    }
    return done;
}

ErrorOr<size_t> TmpFSInode::write(u64 offset, size_t count, IOBuffer const& buffer)
{
    if (m_metadata.type != InodeType::File)
        return Error::from_errno(m_metadata.type == InodeType::Directory ? EISDIR : EINVAL);
    size_t done = 0;
    while (done < count) {
        size_t in_page = (offset + done) % page_size;
        size_t chunk = min(count - done, page_size - in_page);
        PhysicalAddress page;
        {
            MutexLocker locker(tmpfs().lock());
            page = TRY(page_at(offset + done, true));
        }
        auto result = buffer.read(done, { physical_to_virtual(page) + in_page, chunk });
        physical_memory().unref_user_page(page);
        TRY(result);
        done += chunk;
        MutexLocker locker(tmpfs().lock());
        m_metadata.size = max<u64>(m_metadata.size, offset + done);
        m_metadata.modification_time = Time::wall_clock_seconds();
    }
    return done;
}

ErrorOr<void> TmpFSInode::truncate(u64 size)
{
    if (m_metadata.type != InodeType::File)
        return Error::from_errno(m_metadata.type == InodeType::Directory ? EISDIR : EINVAL);
    MutexLocker locker(tmpfs().lock());
    if (size < m_metadata.size) {
        free_pages_from((size + page_size - 1) / page_size);
        // Clear the tail of the last page, so growing again reads zeros.
        if (size % page_size && size / page_size < m_pages.size() && m_pages[size / page_size].get())
            __builtin_memset(physical_to_virtual(m_pages[size / page_size]) + size % page_size, 0, page_size - size % page_size);
    }
    m_metadata.size = size;
    m_metadata.modification_time = Time::wall_clock_seconds();
    return {};
}

TmpFSInode::Child* TmpFSInode::find_child(StringView name)
{
    for (auto& child : m_children) {
        if (child.name == name)
            return &child;
    }
    return nullptr;
}

ErrorOr<NonnullRefPtr<Inode>> TmpFSInode::lookup(StringView name)
{
    if (m_metadata.type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);
    MutexLocker locker(tmpfs().lock());
    if (name == "."_sv)
        return NonnullRefPtr<Inode>(*this);
    if (name == ".."_sv)
        return NonnullRefPtr<Inode>(m_parent ? static_cast<Inode&>(*m_parent) : static_cast<Inode&>(*this));
    if (auto* child = find_child(name))
        return NonnullRefPtr<Inode>(*child->inode);
    return Error::from_errno(ENOENT);
}

ErrorOr<void> TmpFSInode::traverse(DirectoryVisitor& visitor)
{
    if (m_metadata.type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);
    MutexLocker locker(tmpfs().lock());
    if (visitor.visit("."_sv, index(), InodeType::Directory) == VisitDecision::Stop)
        return {};
    if (visitor.visit(".."_sv, m_parent ? m_parent->index() : index(), InodeType::Directory) == VisitDecision::Stop)
        return {};
    for (auto& child : m_children) {
        if (visitor.visit(child.name.view(), child.inode->index(), child.inode->m_metadata.type) == VisitDecision::Stop)
            break;
    }
    return {};
}

ErrorOr<NonnullRefPtr<Inode>> TmpFSInode::create_child(StringView name, InodeType type, u32 permissions, StringView link_target)
{
    if (m_metadata.type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);
    if (name.length() > 255)
        return Error::from_errno(ENAMETOOLONG);
    MutexLocker locker(tmpfs().lock());
    if (find_child(name))
        return Error::from_errno(EEXIST);
    auto inode = TRY(create(tmpfs(), type, permissions));
    if (type == InodeType::SymbolicLink) {
        inode->m_link_target = TRY(String::from_view(link_target));
        inode->m_metadata.size = link_target.length();
    }
    inode->m_parent = this;
    TRY(m_children.try_append(Child { TRY(String::from_view(name)), inode }));
    if (type == InodeType::Directory)
        ++m_metadata.link_count;
    m_metadata.modification_time = Time::wall_clock_seconds();
    return NonnullRefPtr<Inode>(*inode);
}

ErrorOr<void> TmpFSInode::remove_child(StringView name)
{
    if (m_metadata.type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);
    MutexLocker locker(tmpfs().lock());
    auto* child = find_child(name);
    if (!child)
        return Error::from_errno(ENOENT);
    if (child->inode->m_metadata.type == InodeType::Directory)
        --m_metadata.link_count;
    else if (child->inode->m_metadata.link_count > 0)
        --child->inode->m_metadata.link_count;
    m_children.remove_all_matching([&](auto const& candidate) { return candidate.name == name; });
    m_metadata.modification_time = Time::wall_clock_seconds();
    return {};
}

ErrorOr<void> TmpFSInode::add_child(StringView name, Inode& inode)
{
    if (&inode.file_system() != &file_system())
        return Error::from_errno(EXDEV);
    MutexLocker locker(tmpfs().lock());
    if (find_child(name))
        return Error::from_errno(EEXIST);
    auto& tmpfs_inode = static_cast<TmpFSInode&>(inode);
    if (tmpfs_inode.m_metadata.type == InodeType::Directory)
        return Error::from_errno(EPERM);
    TRY(m_children.try_append(Child { TRY(String::from_view(name)), tmpfs_inode }));
    ++tmpfs_inode.m_metadata.link_count;
    return {};
}

ErrorOr<size_t> TmpFSInode::read_link(Bytes buffer)
{
    if (m_metadata.type != InodeType::SymbolicLink)
        return Error::from_errno(EINVAL);
    MutexLocker locker(tmpfs().lock());
    size_t length = min(buffer.size(), m_link_target.length());
    __builtin_memcpy(buffer.data(), m_link_target.characters(), length);
    return length;
}

ErrorOr<void> TmpFSInode::set_owner(u32 uid, u32 gid)
{
    MutexLocker locker(tmpfs().lock());
    m_metadata.uid = uid;
    m_metadata.gid = gid;
    return {};
}

ErrorOr<void> TmpFSInode::set_times(i64 access_time, i64 modification_time)
{
    MutexLocker locker(tmpfs().lock());
    m_metadata.access_time_ns = access_time;
    m_metadata.modification_time_ns = modification_time;
    m_metadata.modification_time = modification_time / 1'000'000'000;
    return {};
}

ErrorOr<void> TmpFSInode::set_permissions(u32 permissions)
{
    MutexLocker locker(tmpfs().lock());
    m_metadata.permissions = permissions & 07777;
    return {};
}

}
