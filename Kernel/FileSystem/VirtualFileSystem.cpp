/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Locking/Mutex.h>
#include <LibBase/Vector.h>

namespace Kernel {

static Atomic<u32> s_next_file_system_id { 1 };

FileSystem::FileSystem()
    : m_id(s_next_file_system_id.fetch_add(1))
{
}

ErrorOr<void> FileSystem::rename(Inode& old_parent, StringView old_name, Inode& new_parent, StringView new_name)
{
    auto inode = TRY(old_parent.lookup(old_name));
    if (auto existing = new_parent.lookup(new_name); !existing.is_error()) {
        if (existing.value().ptr() == inode.ptr())
            return {};
        if (existing.value()->is_directory() && !TRY(existing.value()->is_empty_directory()))
            return Error::from_errno(ENOTEMPTY);
        TRY(new_parent.remove_child(new_name));
    }
    TRY(new_parent.add_child(new_name, *inode));
    return old_parent.remove_child(old_name);
}

namespace {

struct Mount {
    FileSystem* host_file_system;
    InodeIndex host_inode;
    NonnullRefPtr<FileSystem> file_system;
};

Mutex s_mounts_lock;
RefPtr<FileSystem> s_root;
Vector<Mount> s_mounts;

struct Step {
    NonnullRefPtr<Inode> inode;
    String name;
};

// The filesystem mounted on `inode`, if any, replaces it.
NonnullRefPtr<Inode> cross_mount(NonnullRefPtr<Inode> inode)
{
    MutexLocker locker(s_mounts_lock);
    for (auto const& mount : s_mounts) {
        if (mount.host_file_system == &inode->file_system() && mount.host_inode == inode->index())
            return mount.file_system->root_inode();
    }
    return inode;
}

ErrorOr<void> split_components(StringView path, Vector<String>& components)
{
    size_t start = 0;
    for (size_t i = 0; i <= path.length(); ++i) {
        if (i == path.length() || path[i] == '/') {
            if (i > start)
                TRY(components.try_append(TRY(String::from_view(path.substring_view(start, i - start)))));
            start = i + 1;
        }
    }
    return {};
}

ErrorOr<String> join(Vector<Step> const& steps, size_t count)
{
    if (count <= 1)
        return String::from_view("/"_sv);
    StringBuilder builder;
    for (size_t i = 1; i < count; ++i) {
        builder.append('/');
        builder.append(steps[i].name.view());
    }
    return builder.to_string();
}

}

void VirtualFileSystem::mount_root(NonnullRefPtr<FileSystem> file_system)
{
    MutexLocker locker(s_mounts_lock);
    s_root = file_system;
}

bool VirtualFileSystem::has_root()
{
    return !s_root.is_null();
}

ErrorOr<void> VirtualFileSystem::mount(NonnullRefPtr<FileSystem> file_system, StringView path)
{
    auto target = TRY(resolve(path, "/"_sv));
    if (!target.inode->is_directory())
        return Error::from_errno(ENOTDIR);
    MutexLocker locker(s_mounts_lock);
    TRY(s_mounts.try_append(Mount { &target.inode->file_system(), target.inode->index(), file_system }));
    return {};
}

ErrorOr<ResolvedPath> VirtualFileSystem::resolve(StringView path, StringView cwd, bool follow_last_link)
{
    if (path.is_empty())
        return Error::from_errno(ENOENT);
    if (path.length() >= max_path)
        return Error::from_errno(ENAMETOOLONG);
    if (!s_root)
        return Error::from_errno(ENOENT);

    Vector<String> pending;
    if (path[0] != '/')
        TRY(split_components(cwd, pending));
    TRY(split_components(path, pending));

    Vector<Step> steps;
    TRY(steps.try_append(Step { s_root->root_inode(), String() }));
    size_t links_followed = 0;
    size_t index = 0;
    while (index < pending.size()) {
        auto name = pending[index].view();
        bool is_last = index + 1 == pending.size();
        if (name == "."_sv) {
            ++index;
            continue;
        }
        if (name == ".."_sv) {
            if (steps.size() > 1)
                (void)steps.take_last();
            ++index;
            continue;
        }
        auto& directory = steps.last().inode;
        if (!directory->is_directory())
            return Error::from_errno(ENOTDIR);
        auto child = cross_mount(TRY(directory->lookup(name)));

        if (child->metadata().type == InodeType::SymbolicLink && (!is_last || follow_last_link)) {
            if (++links_followed > max_symbolic_links)
                return Error::from_errno(ELOOP);
            char target_buffer[max_path];
            size_t length = TRY(child->read_link({ reinterpret_cast<u8*>(target_buffer), sizeof(target_buffer) }));
            StringView target { target_buffer, length };
            // The link's target replaces it in what is left of the path.
            Vector<String> rest;
            TRY(split_components(target, rest));
            for (size_t i = index + 1; i < pending.size(); ++i)
                TRY(rest.try_append(move(pending[i])));
            pending = move(rest);
            index = 0;
            if (target.starts_with("/"_sv)) {
                while (steps.size() > 1)
                    (void)steps.take_last();
            }
            continue;
        }
        TRY(steps.try_append(Step { child, TRY(String::from_view(name)) }));
        ++index;
    }
    return ResolvedPath { steps.last().inode, TRY(join(steps, steps.size())) };
}

ErrorOr<ResolvedParent> VirtualFileSystem::resolve_parent(StringView path, StringView cwd)
{
    if (path.is_empty())
        return Error::from_errno(ENOENT);
    // Trailing slashes name the same entry.
    while (path.length() > 1 && path.ends_with("/"_sv))
        path = path.substring_view(0, path.length() - 1);
    auto slash = path.find_last('/');
    StringView directory = slash.has_value() ? (slash.value() == 0 ? "/"_sv : path.substring_view(0, slash.value())) : "."_sv;
    StringView name = slash.has_value() ? path.substring_view(slash.value() + 1) : path;
    if (name.is_empty() || name == "."_sv || name == ".."_sv)
        return Error::from_errno(EINVAL);
    auto resolved = TRY(resolve(directory, cwd));
    if (!resolved.inode->is_directory())
        return Error::from_errno(ENOTDIR);
    return ResolvedParent { resolved.inode, move(resolved.path), TRY(String::from_view(name)) };
}

ErrorOr<NonnullRefPtr<FileDescription>> VirtualFileSystem::open(StringView path, StringView cwd, int flags, u32 permissions)
{
    auto resolved = resolve(path, cwd, !(flags & O_NOFOLLOW_INTERNAL));
    if (resolved.is_error()) {
        if (resolved.error().code() != ENOENT || !(flags & O_CREAT))
            return resolved.release_error();
        auto parent = TRY(resolve_parent(path, cwd));
        auto inode = TRY(parent.directory->create_child(parent.name.view(), InodeType::File, permissions & 07777, {}));
        return FileDescription::create(inode, flags);
    }
    if ((flags & O_CREAT) && (flags & O_EXCL))
        return Error::from_errno(EEXIST);
    auto& inode = resolved.value().inode;
    auto metadata = inode->metadata();
    if ((flags & O_DIRECTORY) && metadata.type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);
    if (metadata.type == InodeType::Directory && (flags & O_ACCMODE) != O_RDONLY)
        return Error::from_errno(EISDIR);
    if ((flags & O_ACCMODE) != O_RDONLY && inode->file_system().is_read_only() && metadata.type == InodeType::File)
        return Error::from_errno(EROFS);
    if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY && metadata.type == InodeType::File)
        TRY(inode->truncate(0));
    return FileDescription::create(TRY(inode->open_inode(flags)), flags);
}

ErrorOr<void> VirtualFileSystem::mkdir(StringView path, StringView cwd, u32 permissions)
{
    auto parent = TRY(resolve_parent(path, cwd));
    if (!parent.directory->lookup(parent.name.view()).is_error())
        return Error::from_errno(EEXIST);
    (void)TRY(parent.directory->create_child(parent.name.view(), InodeType::Directory, permissions & 07777, {}));
    return {};
}

ErrorOr<void> VirtualFileSystem::unlink(StringView path, StringView cwd)
{
    auto parent = TRY(resolve_parent(path, cwd));
    auto inode = TRY(parent.directory->lookup(parent.name.view()));
    if (inode->is_directory())
        return Error::from_errno(EISDIR);
    return parent.directory->remove_child(parent.name.view());
}

ErrorOr<void> VirtualFileSystem::rmdir(StringView path, StringView cwd)
{
    auto parent = TRY(resolve_parent(path, cwd));
    auto inode = cross_mount(TRY(parent.directory->lookup(parent.name.view())));
    if (!inode->is_directory())
        return Error::from_errno(ENOTDIR);
    if (&inode->file_system() != &parent.directory->file_system())
        return Error::from_errno(EBUSY); // A mount point.
    if (!TRY(inode->is_empty_directory()))
        return Error::from_errno(ENOTEMPTY);
    return parent.directory->remove_child(parent.name.view());
}

ErrorOr<void> VirtualFileSystem::rename(StringView old_path, StringView new_path, StringView cwd)
{
    auto old_parent = TRY(resolve_parent(old_path, cwd));
    auto new_parent = TRY(resolve_parent(new_path, cwd));
    if (&old_parent.directory->file_system() != &new_parent.directory->file_system())
        return Error::from_errno(18); // EXDEV
    auto moving = TRY(old_parent.directory->lookup(old_parent.name.view()));
    // A directory cannot move inside itself.
    if (moving->is_directory()) {
        auto moving_path = TRY(resolve(old_path, cwd, false));
        auto destination = new_parent.directory_path.view();
        if (destination == moving_path.path.view() || (destination.starts_with(moving_path.path.view()) && destination[moving_path.path.length()] == '/'))
            return Error::from_errno(EINVAL);
    }
    return old_parent.directory->file_system().rename(*old_parent.directory, old_parent.name.view(), *new_parent.directory, new_parent.name.view());
}

ErrorOr<void> VirtualFileSystem::symlink(StringView target, StringView link_path, StringView cwd)
{
    auto parent = TRY(resolve_parent(link_path, cwd));
    if (!parent.directory->lookup(parent.name.view()).is_error())
        return Error::from_errno(EEXIST);
    (void)TRY(parent.directory->create_child(parent.name.view(), InodeType::SymbolicLink, 0777, target));
    return {};
}

ErrorOr<void> VirtualFileSystem::sync_all()
{
    Vector<NonnullRefPtr<FileSystem>> all;
    {
        MutexLocker locker(s_mounts_lock);
        if (s_root)
            TRY(all.try_append(*s_root));
        for (auto const& mount : s_mounts)
            TRY(all.try_append(mount.file_system));
    }
    for (auto& file_system : all)
        TRY(file_system->sync());
    return {};
}

}
