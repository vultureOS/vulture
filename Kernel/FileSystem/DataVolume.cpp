/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/files.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/FileSystem/DataVolume.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/Heap.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

namespace Kernel::DataVolume {

namespace {

constexpr StringView data_path = "/System/Volumes/Data"_sv;
constexpr StringView template_path = "/System/Library/Templates/Data"_sv;
constexpr StringView firmlinks_path = "/usr/share/firmlinks"_sv;

struct Child {
    String name;
    InodeType type;
};

ErrorOr<Vector<Child>> children_of(Inode& directory)
{
    class Lister final : public DirectoryVisitor {
    public:
        VisitDecision visit(StringView name, InodeIndex, InodeType type) override
        {
            if (name == "."_sv || name == ".."_sv)
                return VisitDecision::Continue;
            auto copy = String::from_view(name);
            if (copy.is_error() || children.try_append(Child { copy.release_value(), type }).is_error()) {
                failed = true;
                return VisitDecision::Stop;
            }
            return VisitDecision::Continue;
        }
        Vector<Child> children;
        bool failed { false };
    };
    Lister lister;
    TRY(directory.traverse(lister));
    if (lister.failed)
        return Error::from_errno(ENOMEM);
    return move(lister.children);
}

ErrorOr<String> joined(StringView directory, StringView name)
{
    StringBuilder path;
    path.append(directory);
    if (!directory.ends_with("/"_sv))
        path.append('/');
    path.append(name);
    return path.to_string();
}

ErrorOr<void> copy_file(Inode& from, StringView to, u32 mode, u32 uid, u32 gid)
{
    auto file = TRY(VirtualFileSystem::open(to, "/"_sv, O_WRONLY | O_CREAT | O_EXCL, mode));
    auto* inode = file->inode();
    if (!inode)
        return Error::from_errno(EIO);
    constexpr size_t chunk_size = 64 * KiB;
    auto* chunk = static_cast<u8*>(kmalloc(chunk_size));
    if (!chunk)
        return Error::from_errno(ENOMEM);
    ErrorOr<void> result = {};
    u64 const size = from.metadata().size;
    for (u64 done = 0; done < size;) {
        auto got = from.read(done, min<u64>(chunk_size, size - done), IOBuffer::kernel(chunk));
        if (got.is_error()) {
            result = got.release_error();
            break;
        }
        if (got.value() == 0)
            break;
        auto written = inode->write(done, got.value(), IOBuffer::kernel(chunk));
        if (written.is_error()) {
            result = written.release_error();
            break;
        }
        done += got.value();
    }
    kfree(chunk);
    TRY(result);
    TRY(inode->set_permissions(mode));
    return inode->set_owner(uid, gid);
}

// What's in `from` that's not at `to`; what's there is the user's, and stays as it is.
ErrorOr<size_t> copy_what_is_missing(Inode& from, StringView to)
{
    size_t copied = 0;
    auto const children = TRY(children_of(from));
    for (auto const& child : children) {
        auto inode = TRY(from.lookup(child.name.view()));
        auto const metadata = inode->metadata();
        auto const path = TRY(joined(to, child.name.view()));
        auto const existing = VirtualFileSystem::resolve(path.view(), "/"_sv, false);
        if (child.type == InodeType::Directory) {
            if (existing.is_error()) {
                TRY(VirtualFileSystem::mkdir(path.view(), "/"_sv, metadata.permissions));
                auto made = TRY(VirtualFileSystem::resolve(path.view(), "/"_sv));
                TRY(made.inode->set_permissions(metadata.permissions));
                TRY(made.inode->set_owner(metadata.uid, metadata.gid));
                ++copied;
            }
            copied += TRY(copy_what_is_missing(*inode, path.view()));
        } else if (existing.is_error()) {
            if (child.type == InodeType::SymbolicLink) {
                char target[VirtualFileSystem::max_path];
                size_t const length = TRY(inode->read_link({ reinterpret_cast<u8*>(target), sizeof(target) }));
                TRY(VirtualFileSystem::symlink({ target, length }, path.view(), "/"_sv));
            } else {
                TRY(copy_file(*inode, path.view(), metadata.permissions, metadata.uid, metadata.gid));
            }
            ++copied;
        }
    }
    return copied;
}

void empty_directory(Inode& directory, StringView path)
{
    auto children = children_of(directory);
    if (children.is_error())
        return;
    for (auto const& child : children.value()) {
        auto inner = joined(path, child.name.view());
        if (inner.is_error())
            continue;
        if (child.type == InodeType::Directory) {
            auto inode = directory.lookup(child.name.view());
            if (!inode.is_error())
                empty_directory(*inode.value(), inner.value().view());
            (void)VirtualFileSystem::rmdir(inner.value().view(), "/"_sv);
        } else {
            (void)VirtualFileSystem::unlink(inner.value().view(), "/"_sv);
        }
    }
}

ErrorOr<void> make_directories(StringView path)
{
    for (size_t i = 1; i <= path.length(); ++i) {
        if (i != path.length() && path[i] != '/')
            continue;
        auto const part = path.substring_view(0, i);
        if (VirtualFileSystem::resolve(part, "/"_sv).is_error())
            TRY(VirtualFileSystem::mkdir(part, "/"_sv, 0755));
    }
    return {};
}

// "/Users<tab>Users", as a Mac's says.
ErrorOr<size_t> make_firmlinks(FileSystem& volume)
{
    auto const file = TRY(VirtualFileSystem::resolve(firmlinks_path, "/"_sv));
    u64 const size = file.inode->metadata().size;
    auto* bytes = static_cast<char*>(kmalloc(size + 1));
    if (!bytes)
        return Error::from_errno(ENOMEM);
    auto const got = file.inode->read(0, size, IOBuffer::kernel(bytes));
    if (got.is_error()) {
        kfree(bytes);
        return got.error();
    }
    StringView const text { bytes, got.value() };
    size_t count = 0;
    ErrorOr<void> result = {};
    text.for_each_split_view('\n', SplitBehavior::SkipEmpty, [&](StringView line) {
        auto const tab = line.find('\t');
        if (result.is_error() || !tab.has_value())
            return;
        auto const where = line.substring_view(0, tab.value());
        auto const inner = line.substring_view(tab.value() + 1);
        auto const on_volume = joined(data_path, inner);
        if (on_volume.is_error()) {
            result = on_volume.error();
            return;
        }
        result = make_directories(on_volume.value().view());
        if (result.is_error())
            return;
        auto const directory = VirtualFileSystem::resolve(on_volume.value().view(), "/"_sv);
        if (directory.is_error()) {
            result = directory.error();
            return;
        }
        // Hidden, if what it stands in for on the System volume is (as /private and /Volumes are): what's seen is this one.
        if (auto const placeholder = VirtualFileSystem::resolve(where, "/"_sv); !placeholder.is_error()) {
            u32 const hidden = placeholder.value().inode->metadata().flags & UF_HIDDEN;
            auto& target = *directory.value().inode;
            if (hidden && !(target.metadata().flags & UF_HIDDEN))
                (void)target.set_flags((target.metadata().flags & ~UF_COMPRESSED) | UF_HIDDEN);
        }
        result = VirtualFileSystem::firmlink(where, volume, directory.value().inode);
        ++count;
    });
    kfree(bytes);
    TRY(result);
    return count;
}

}

ErrorOr<void> attach(NonnullRefPtr<FileSystem> volume)
{
    auto& file_system = *volume;
    TRY(VirtualFileSystem::mount(move(volume), data_path));
    size_t copied = 0;
    if (auto const from = VirtualFileSystem::resolve(template_path, "/"_sv); !from.is_error())
        copied = TRY(copy_what_is_missing(*from.value().inode, data_path));
    size_t const firmlinks = TRY(make_firmlinks(file_system));
    auto const temporary = TRY(VirtualFileSystem::resolve("/private/tmp"_sv, "/"_sv));
    empty_directory(*temporary.inode, "/private/tmp"_sv);
    dmesgln("Data: {} things new, {} firmlinks", copied, firmlinks);
    return {};
}

}
