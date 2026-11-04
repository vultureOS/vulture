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

// What a system has that's empty: mount points, and where the data volume shows through (usr/share/firmlinks).
struct SkeletonDirectory {
    StringView path;
    u32 mode;
};
SkeletonDirectory const s_skeleton[] = {
    { "Applications"_sv, 0755 },
    { "Library"_sv, 0755 },
    { "System"_sv, 0755 },
    { "System/Applications"_sv, 0755 },
    { "System/Library"_sv, 0755 },
    { "System/Library/Caches"_sv, 0755 },
    { "System/Volumes"_sv, 0755 },
    { "System/Volumes/Data"_sv, 0755 },
    { "Users"_sv, 0755 },
    { "Volumes"_sv, 0755 },
    { "cores"_sv, 01777 },
    { "dev"_sv, 0755 },
    { "opt"_sv, 0755 },
    { "private"_sv, 0755 },
    { "usr"_sv, 0755 },
    { "usr/local"_sv, 0755 },
};

constexpr StringView data_template = "System/Library/Templates/Data"_sv;

// Where something from the build goes on a system volume, if it does.
ErrorOr<String> place_on_system_volume(StringView path)
{
    auto is_under = [&](StringView root) {
        return path == root || (path.starts_with(root) && path.length() > root.length() && path[root.length()] == '/');
    };
    auto moved = [&](StringView before, StringView after) -> ErrorOr<String> {
        StringBuilder place;
        place.append(after);
        place.append(path.substring_view(before.length()));
        return place.to_string();
    };
    if (path == "etc/ssl/cert.pem"_sv)
        return String::from_view("System/Library/Security/cert.pem"_sv);
    if (is_under("Applications"_sv))
        return moved("Applications"_sv, "System/Applications"_sv);
    StringBuilder under_template;
    under_template.append(data_template);
    under_template.append('/');
    StringView const to_private[] = { "etc"_sv, "var"_sv, "tmp"_sv };
    for (auto root : to_private) {
        if (is_under(root)) {
            under_template.append("private/"_sv);
            under_template.append(path);
            return under_template.to_string();
        }
    }
    StringView const to_template[] = { "Users"_sv, "Library"_sv, "Volumes"_sv, "cores"_sv, "opt"_sv, "usr/local"_sv };
    for (auto root : to_template) {
        if (is_under(root)) {
            under_template.append(path);
            return under_template.to_string();
        }
    }
    return String::from_view(path);
}

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
    auto const place = TRY(place_on_system_volume(entry.path));
    StringView parent_path, name = place.view();
    if (auto slash = place.view().find_last('/'); slash.has_value()) {
        parent_path = place.view().substring_view(0, slash.value());
        name = place.view().substring_view(slash.value() + 1);
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

ErrorOr<void> make_system_volume(Volume& volume, ReadonlyBytes archive, PopulateCounts& counts)
{
    for (auto const& directory : s_skeleton) {
        auto made = TRY(make_directories(volume, directory.path, counts));
        TRY(volume.set_permissions(made, directory.mode));
    }
    // As a Mac has them: what looks like it's at the top is really among the data.
    struct Link {
        StringView name;
        StringView target;
    };
    Link const links[] = { { "etc"_sv, "private/etc"_sv }, { "tmp"_sv, "private/tmp"_sv }, { "var"_sv, "private/var"_sv } };
    for (auto const& link : links) {
        (void)TRY(volume.create(root_directory, link.name, mode_symbolic_link | 0755, 0, 0, link.target));
        ++counts.links;
    }

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
    TRY(result);

    // Whoever looks in the usual place for the certificates finds the sealed ones.
    StringBuilder ssl;
    ssl.append(data_template);
    ssl.append("/private/etc/ssl"_sv);
    auto const ssl_directory = TRY(make_directories(volume, ssl.view(), counts));
    (void)TRY(volume.create(ssl_directory, "cert.pem"_sv, mode_symbolic_link | 0755, 0, 0, "/System/Library/Security/cert.pem"_sv));
    return {};
}

u64 estimated_system_blocks(ReadonlyBytes archive)
{
    u64 blocks = 4096;
    auto reader = Archive::TarReader::parse(archive);
    if (reader.is_error())
        return blocks;
    (void)reader.value().for_each_entry([&](Archive::TarEntry const& entry) {
        blocks += (entry.data.size() + block_size - 1) / block_size + 1;
        return Archive::IterationDecision::Continue;
    });
    return blocks + blocks / 8;
}

ErrorOr<u64> system_partition_sectors(u64 image_size, u64 sectors_available)
{
    u64 const image_sectors = (image_size + block_size - 1) / block_size * (block_size / sector_size);
    // Room to grow, so a kept disk takes a good many updates; but a disk may be small.
    u64 wanted = max<u64>(image_sectors * 2, 1024 * MiB / sector_size);
    if (wanted > sectors_available / 2)
        wanted = max<u64>(image_sectors, sectors_available / 2);
    wanted = wanted / 8 * 8;
    if (image_sectors + 2048 > sectors_available || wanted >= sectors_available)
        return Error::from_string_literal("that disk is too small for this system");
    return wanted;
}

ErrorOr<void> write_system(FILE* file, u64 first_sector, u64 sector_count, ReadonlyBytes image)
{
    if (image.size() > sector_count * sector_size)
        return Error::from_string_literal("there isn't room for this system");
    if (fseek(file, static_cast<long>(first_sector * sector_size), 0) != 0 || fwrite(image.data(), 1, image.size(), file) != image.size())
        return Error::from_errno(EIO);
    return {};
}

ErrorOr<void> format_data(FILE* file, u64 first_sector, u64 sector_count, StringView name)
{
    ImageStore store(file, first_sector * sector_size, sector_count * sector_size / block_size);
    u8 uuid[16];
    random_bytes(uuid);
    FormatOptions options;
    options.role = role_data;
    auto volume = TRY(Volume::format(store, name, uuid, host_clock, options));
    return volume->checkpoint();
}

}
