/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/Security/Privacy.h>
#include <Kernel/Security/Sandbox.h>
#include <Kernel/Security/Seal.h>
#include <LibBase/Vector.h>

namespace Kernel {

static constexpr int read_access = 4;
static constexpr int write_access = 2;
static constexpr int search_access = 1;

// A new file belongs to whoever made it, in the group of its directory if
// that is set-group-ID (as BSD does), else the maker's.
static void assign_owner(Inode& created, Inode& directory)
{
    auto const* credentials = current_credentials();
    if (!credentials)
        return;
    auto parent = directory.metadata();
    u32 group = (parent.permissions & 02000) ? parent.gid : credentials->egid;
    (void)created.set_owner(credentials->euid, group);
}

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
    String path;
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

// What a sandbox sees: where something in `parent` would be, or is.
ErrorOr<void> check_may_write_entry(ResolvedParent const& parent)
{
    StringBuilder path;
    if (parent.directory_path.view() != "/"_sv)
        path.append(parent.directory_path.view());
    path.append('/');
    path.append(parent.name.view());
    TRY(sandbox_check(SandboxOperation::FileWrite, path.view()));
    return Privacy::check(path.view());
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
    TRY(s_mounts.try_append(Mount { &target.inode->file_system(), target.inode->index(), file_system, move(target.path) }));
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
        auto directory_metadata = directory->metadata();
        if (directory_metadata.type != InodeType::Directory)
            return Error::from_errno(ENOTDIR);
        // Going through a directory takes search (execute) permission.
        TRY(check_access(directory_metadata, search_access));
        auto looked_up = TRY(directory->lookup(name));
        TRY(Seal::check_child(*directory, *looked_up));
        auto child = cross_mount(looked_up);

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
        TRY(check_may_write_entry(parent));
        TRY(Seal::check_may_change(*parent.directory));
        TRY(check_access(parent.directory->metadata(), write_access | search_access));
        auto inode = TRY(parent.directory->create_child(parent.name.view(), InodeType::File, permissions & 07777, {}));
        assign_owner(*inode, *parent.directory);
        return FileDescription::create(inode, flags);
    }
    if ((flags & O_CREAT) && (flags & O_EXCL))
        return Error::from_errno(EEXIST);
    auto& inode = resolved.value().inode;
    auto metadata = inode->metadata();
    if ((flags & O_ACCMODE) != O_WRONLY)
        TRY(sandbox_check(SandboxOperation::FileRead, resolved.value().path.view()));
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
        TRY(sandbox_check(SandboxOperation::FileWrite, resolved.value().path.view()));
    TRY(Privacy::check(resolved.value().path.view()));
    // O_NOFOLLOW: a symbolic link at the end is an error, not a file.
    if ((flags & O_NOFOLLOW) && metadata.type == InodeType::SymbolicLink)
        return Error::from_errno(ELOOP);
    if ((flags & O_DIRECTORY) && metadata.type != InodeType::Directory)
        return Error::from_errno(ENOTDIR);
    if (metadata.type == InodeType::Directory && (flags & O_ACCMODE) != O_RDONLY)
        return Error::from_errno(EISDIR);
    if ((flags & O_ACCMODE) != O_RDONLY && inode->file_system().is_read_only() && metadata.type == InodeType::File)
        return Error::from_errno(EROFS);
    bool const wants_to_write = (flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC);
    if (wants_to_write) {
        TRY(Seal::check_may_change(*inode));
        // Nor around the filesystem, to the disk underneath it.
        if (metadata.type == InodeType::BlockDevice && Seal::protects_block_device(inode->block_device_name()))
            return Error::from_errno(EPERM);
    } else if (metadata.type == InodeType::File) {
        TRY(Seal::check_contents(*inode));
    }
    int wanted = 0;
    if ((flags & O_ACCMODE) != O_WRONLY)
        wanted |= read_access;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
        wanted |= write_access;
    TRY(check_access(metadata, wanted));
    if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY && metadata.type == InodeType::File)
        TRY(inode->truncate(0));
    return FileDescription::create(TRY(inode->open_inode(flags)), flags);
}

ErrorOr<void> VirtualFileSystem::mkdir(StringView path, StringView cwd, u32 permissions)
{
    auto parent = TRY(resolve_parent(path, cwd));
    if (!parent.directory->lookup(parent.name.view()).is_error())
        return Error::from_errno(EEXIST);
    TRY(check_may_write_entry(parent));
    TRY(Seal::check_may_change(*parent.directory));
    TRY(check_access(parent.directory->metadata(), write_access | search_access));
    auto directory = TRY(parent.directory->create_child(parent.name.view(), InodeType::Directory, permissions & 07777, {}));
    assign_owner(*directory, *parent.directory);
    return {};
}

ErrorOr<void> VirtualFileSystem::unlink(StringView path, StringView cwd)
{
    auto parent = TRY(resolve_parent(path, cwd));
    auto inode = TRY(parent.directory->lookup(parent.name.view()));
    auto metadata = inode->metadata();
    if (metadata.type == InodeType::Directory)
        return Error::from_errno(EISDIR);
    TRY(check_may_write_entry(parent));
    TRY(Seal::check_may_change(*parent.directory));
    TRY(Seal::check_may_change(*inode));
    TRY(check_removal(parent.directory->metadata(), metadata));
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
    TRY(check_may_write_entry(parent));
    TRY(Seal::check_may_change(*parent.directory));
    TRY(Seal::check_may_change(*inode));
    if (!TRY(inode->is_empty_directory()))
        return Error::from_errno(ENOTEMPTY);
    TRY(check_removal(parent.directory->metadata(), inode->metadata()));
    return parent.directory->remove_child(parent.name.view());
}

ErrorOr<void> VirtualFileSystem::rename(StringView old_path, StringView new_path, StringView cwd)
{
    auto old_parent = TRY(resolve_parent(old_path, cwd));
    auto new_parent = TRY(resolve_parent(new_path, cwd));
    if (&old_parent.directory->file_system() != &new_parent.directory->file_system())
        return Error::from_errno(EXDEV);
    auto moving = TRY(old_parent.directory->lookup(old_parent.name.view()));
    TRY(check_may_write_entry(old_parent));
    TRY(check_may_write_entry(new_parent));
    TRY(Seal::check_may_change(*old_parent.directory));
    TRY(Seal::check_may_change(*new_parent.directory));
    TRY(Seal::check_may_change(*moving));
    TRY(check_removal(old_parent.directory->metadata(), moving->metadata()));
    TRY(check_access(new_parent.directory->metadata(), write_access | search_access));
    if (auto existing = new_parent.directory->lookup(new_parent.name.view()); !existing.is_error()) {
        TRY(Seal::check_may_change(*existing.value()));
        TRY(check_removal(new_parent.directory->metadata(), existing.value()->metadata()));
    }
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
    TRY(check_may_write_entry(parent));
    TRY(Seal::check_may_change(*parent.directory));
    TRY(check_access(parent.directory->metadata(), write_access | search_access));
    auto link = TRY(parent.directory->create_child(parent.name.view(), InodeType::SymbolicLink, 0777, target));
    assign_owner(*link, *parent.directory);
    return {};
}

ErrorOr<void> VirtualFileSystem::link(StringView existing_path, StringView new_path, StringView cwd)
{
    auto existing = TRY(resolve(existing_path, cwd, false));
    if (existing.inode->is_directory())
        return Error::from_errno(EPERM);
    auto parent = TRY(resolve_parent(new_path, cwd));
    if (&parent.directory->file_system() != &existing.inode->file_system())
        return Error::from_errno(EXDEV);
    if (!parent.directory->lookup(parent.name.view()).is_error())
        return Error::from_errno(EEXIST);
    // A second name for something: only by someone who could change it under its first.
    TRY(sandbox_check(SandboxOperation::FileWrite, existing.path.view()));
    TRY(Privacy::check(existing.path.view()));
    TRY(check_may_write_entry(parent));
    TRY(Seal::check_may_change(*parent.directory));
    // A second name for something sealed, somewhere it could be removed, would still be all right;
    // but a change to its link count isn't what the seal says either.
    TRY(Seal::check_may_change(*existing.inode));
    TRY(check_access(parent.directory->metadata(), write_access | search_access));
    return parent.directory->add_child(parent.name.view(), *existing.inode);
}

ErrorOr<void> VirtualFileSystem::for_each_mount_point(Function callback, void* context)
{
    Vector<NonnullRefPtr<FileSystem>> systems;
    Vector<String> paths;
    {
        MutexLocker locker(s_mounts_lock);
        if (s_root) {
            TRY(systems.try_append(*s_root));
            TRY(paths.try_append(TRY(String::from_view("/"_sv))));
        }
        for (auto const& mount : s_mounts) {
            TRY(systems.try_append(mount.file_system));
            TRY(paths.try_append(mount.path));
        }
    }
    for (size_t i = 0; i < systems.size(); ++i)
        TRY(callback(context, *systems[i], paths[i].view()));
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
