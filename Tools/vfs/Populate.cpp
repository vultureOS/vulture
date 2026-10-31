/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Populate.h"
#include "Image.h"
#include <Kernel/API/POSIX/errno.h>
#include <LibArchive/Tar.h>
#include <LibBase/String.h>

namespace Tools {

using namespace VultureFS;

namespace {

// Mount points and the top of the macOS-style hierarchy, made even when
// the archive has nothing in them.
struct SkeletonDirectory {
    StringView path;
    u32 mode;
};
SkeletonDirectory const s_skeleton[] = {
    { "Applications"_sv, 0755 },
    { "Library"_sv, 0755 },
    { "System"_sv, 0755 },
    { "System/Library"_sv, 0755 },
    { "Users"_sv, 0755 },
    { "Volumes"_sv, 0755 },
    { "dev"_sv, 0755 },
    { "tmp"_sv, 01777 },
};

// Finds the directory `path` names, making any part that is missing.
ErrorOr<InodeNumber> make_directories(Volume& volume, StringView path, PopulateCounts& counts)
{
    InodeNumber directory = root_directory;
    ErrorOr<void> result {};
    path.for_each_split_view('/', SplitBehavior::SkipEmpty, [&](StringView component) {
        if (result.is_error())
            return;
        auto entry = volume.lookup(directory, component);
        if (!entry.is_error()) {
            if (entry.value().type != type_directory) {
                result = Error::from_errno(ENOTDIR);
                return;
            }
            directory = entry.value().inode;
            return;
        }
        auto created = volume.create(directory, component, mode_directory | 0755, 0, 0);
        if (created.is_error()) {
            result = created.release_error();
            return;
        }
        directory = created.value();
        ++counts.directories;
    });
    TRY(result);
    return directory;
}

// Removes whatever `name` is in `directory`, emptying directories first.
ErrorOr<void> remove_tree(Volume& volume, InodeNumber directory, StringView name)
{
    auto entry = TRY(volume.lookup(directory, name));
    if (entry.type == type_directory) {
        Vector<String> names;
        TRY(volume.for_each_entry(entry.inode, [&](StringView child, DirectoryEntry) {
            names.append(MUST(String::from_view(child)));
            return Iteration::Continue;
        }));
        for (auto const& child : names)
            TRY(remove_tree(volume, entry.inode, child.view()));
        auto removed = TRY(volume.remove_directory(directory, name));
        return volume.release(removed.inode);
    }
    auto removed = TRY(volume.unlink(directory, name));
    if (removed.last_link)
        TRY(volume.release(removed.inode));
    return {};
}

ErrorOr<void> add_entry(Volume& volume, Archive::TarEntry const& entry, PopulateCounts& counts)
{
    StringView parent_path, name = entry.path;
    if (auto slash = entry.path.find_last('/'); slash.has_value()) {
        parent_path = entry.path.substring_view(0, slash.value());
        name = entry.path.substring_view(slash.value() + 1);
    }
    auto parent = TRY(make_directories(volume, parent_path, counts));
    auto existing = volume.lookup(parent, name);
    u32 permissions = entry.mode & 07777;

    if (entry.type == Archive::TarEntryType::Directory) {
        if (!existing.is_error() && existing.value().type == type_directory)
            return volume.set_permissions(existing.value().inode, permissions);
        if (!existing.is_error())
            TRY(remove_tree(volume, parent, name));
        (void)TRY(volume.create(parent, name, mode_directory | permissions, 0, 0));
        ++counts.directories;
        return {};
    }

    if (entry.type == Archive::TarEntryType::SymbolicLink) {
        if (!existing.is_error())
            TRY(remove_tree(volume, parent, name));
        (void)TRY(volume.create(parent, name, mode_symbolic_link | 0777, 0, 0, entry.link_target));
        ++counts.links;
        return {};
    }

    if (entry.type != Archive::TarEntryType::File)
        return {};
    InodeNumber file;
    if (!existing.is_error() && existing.value().type == type_regular) {
        file = existing.value().inode;
        TRY(volume.truncate(file, 0));
        TRY(volume.set_permissions(file, permissions));
    } else {
        if (!existing.is_error())
            TRY(remove_tree(volume, parent, name));
        file = TRY(volume.create(parent, name, mode_regular | permissions, 0, 0));
    }
    TRY(volume.write(file, 0, entry.data));
    ++counts.files;
    counts.bytes += entry.data.size();
    return {};
}

}

ErrorOr<void> make_skeleton(Volume& volume, PopulateCounts& counts)
{
    for (auto const& directory : s_skeleton) {
        auto made = TRY(make_directories(volume, directory.path, counts));
        TRY(volume.set_permissions(made, directory.mode));
    }
    return {};
}

ErrorOr<void> add_archive(Volume& volume, ReadonlyBytes archive, PopulateCounts& counts)
{
    auto reader = TRY(Archive::TarReader::parse(archive));
    ErrorOr<void> result {};
    TRY(reader.for_each_entry([&](Archive::TarEntry const& entry) {
        result = add_entry(volume, entry, counts);
        if (result.is_error()) {
            StandardError error;
            format_to(error, "{}: {}\n", entry.path, result.error());
            return Archive::IterationDecision::Break;
        }
        return Archive::IterationDecision::Continue;
    }));
    return result;
}

}
