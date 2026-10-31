/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Image.h"
#include <Kernel/API/POSIX/errno.h>
#include <LibArchive/Tar.h>
#include <LibVultureFS/Check.h>
#include <LibVultureFS/Volume.h>

// mkvfs create IMAGE SIZE_MIB ARCHIVE [NAME]
//     A new GPT disk image with one vultureFS partition, holding the files
//     of the boot archive (a ustar file) plus the usual empty directories.
//
// mkvfs update IMAGE ARCHIVE
//     Brings the files of the archive into an existing image, replacing
//     those it has and leaving everything else (the user's files) alone:
//     a system update.

using namespace VultureFS;

namespace {

template<typename... Args>
int fail(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    Tools::StandardError error;
    error.write("mkvfs: "_sv);
    format_to(error, format, args...);
    error.write("\n"_sv);
    return 1;
}

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

struct Counts {
    size_t files { 0 };
    size_t directories { 0 };
    size_t links { 0 };
    u64 bytes { 0 };
};

// Finds the directory `path` names, making any part that is missing.
ErrorOr<InodeNumber> make_directories(Volume& volume, StringView path, Counts& counts)
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

ErrorOr<void> add_entry(Volume& volume, Archive::TarEntry const& entry, Counts& counts)
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

ErrorOr<void> add_archive(Volume& volume, ReadonlyBytes archive, Counts& counts)
{
    auto reader = TRY(Archive::TarReader::parse(archive));
    ErrorOr<void> result {};
    TRY(reader.for_each_entry([&](Archive::TarEntry const& entry) {
        result = add_entry(volume, entry, counts);
        if (result.is_error()) {
            fail("{}: {}", entry.path, result.error());
            return Archive::IterationDecision::Break;
        }
        return Archive::IterationDecision::Continue;
    }));
    return result;
}

int create(char const* image_path, u64 size_mib, char const* archive_path, StringView name)
{
    auto archive = Tools::read_file(archive_path);
    if (archive.is_error())
        return fail("cannot read {}: {}", StringView(archive_path), archive.error());
    FILE* image = fopen(image_path, "w+b");
    if (!image)
        return fail("cannot create {}", StringView(image_path));
    auto partition = Tools::create_disk_image(image, size_mib * 1024 * 1024, "vultureOS"_sv);
    if (partition.is_error()) {
        fclose(image);
        return fail("{}", partition.error());
    }
    Tools::ImageStore store(image, partition.value().first_sector * Tools::sector_size, partition.value().sector_count * Tools::sector_size / block_size);
    u8 uuid[16];
    Tools::random_bytes(uuid);
    auto volume = Volume::format(store, name, uuid, Tools::host_clock);
    if (volume.is_error()) {
        fclose(image);
        return fail("cannot format: {}", volume.error());
    }
    Counts counts;
    for (auto const& directory : s_skeleton) {
        auto made = make_directories(*volume.value(), directory.path, counts);
        if (made.is_error() || volume.value()->set_permissions(made.value(), directory.mode).is_error()) {
            fclose(image);
            return fail("cannot make {}", directory.path);
        }
    }
    auto added = add_archive(*volume.value(), { archive.value().begin(), archive.value().size() }, counts);
    auto committed = added.is_error() ? added : volume.value()->checkpoint();
    if (committed.is_error()) {
        fclose(image);
        return fail("cannot write the volume: {}", committed.error());
    }
    auto statistics = volume.value()->statistics();
    fclose(image);
    Tools::print("mkvfs: {}: {} MiB, {} files ({} KiB), {} directories, {} links; {} of {} blocks free\n", StringView(image_path), size_mib,
        counts.files, counts.bytes / 1024, counts.directories, counts.links, statistics.free_blocks, statistics.block_count);
    return 0;
}

int update(char const* image_path, char const* archive_path)
{
    auto archive = Tools::read_file(archive_path);
    if (archive.is_error())
        return fail("cannot read {}: {}", StringView(archive_path), archive.error());
    FILE* image = fopen(image_path, "r+b");
    if (!image)
        return fail("cannot open {}", StringView(image_path));
    auto partition = Tools::find_vulturefs_partition(image);
    if (partition.is_error()) {
        fclose(image);
        return fail("{}: {}", StringView(image_path), partition.error());
    }
    Tools::ImageStore store(image, partition.value().first_sector * Tools::sector_size, partition.value().sector_count * Tools::sector_size / block_size);
    auto volume = Volume::mount(store, Tools::host_clock);
    if (volume.is_error()) {
        fclose(image);
        return fail("cannot mount {}: {}", StringView(image_path), volume.error());
    }
    Counts counts;
    auto added = add_archive(*volume.value(), { archive.value().begin(), archive.value().size() }, counts);
    auto committed = added.is_error() ? added : volume.value()->checkpoint();
    fclose(image);
    if (committed.is_error())
        return fail("cannot update the volume: {}", committed.error());
    Tools::print("mkvfs: {}: updated {} files ({} KiB); generation {}\n", StringView(image_path), counts.files, counts.bytes / 1024,
        volume.value()->statistics().generation);
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc >= 5 && StringView(argv[1]) == "create"_sv) {
        auto size = StringView(argv[3]).to_number<u64>();
        if (!size.has_value() || size.value() < 16)
            return fail("size must be a number of MiB, at least 16");
        return create(argv[2], size.value(), argv[4], argc >= 6 ? StringView(argv[5]) : "Vulture HD"_sv);
    }
    if (argc == 4 && StringView(argv[1]) == "update"_sv)
        return update(argv[2], argv[3]);
    return fail("usage: mkvfs create IMAGE SIZE_MIB ARCHIVE [NAME] | mkvfs update IMAGE ARCHIVE");
}
