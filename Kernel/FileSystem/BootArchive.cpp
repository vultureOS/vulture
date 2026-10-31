/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/BootArchive.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <LibArchive/Tar.h>

namespace Kernel {

// mkdir -p for the directories above `path`.
static ErrorOr<void> make_parents(StringView path, UnpackedArchive& result)
{
    for (size_t i = 1; i < path.length(); ++i) {
        if (path[i] != '/')
            continue;
        auto made = VirtualFileSystem::mkdir(path.substring_view(0, i), "/"_sv, 0755);
        if (!made.is_error())
            ++result.directories;
        else if (made.error().code() != EEXIST)
            return made.release_error();
    }
    return {};
}

ErrorOr<UnpackedArchive> unpack_boot_archive(ReadonlyBytes archive)
{
    auto reader = TRY(Archive::TarReader::parse(archive));
    UnpackedArchive result;
    Optional<Error> failure;
    TRY(reader.for_each_entry([&](Archive::TarEntry const& entry) {
        char path_buffer[VirtualFileSystem::max_path];
        if (entry.path.is_empty() || entry.path.length() + 2 > sizeof(path_buffer))
            return Archive::IterationDecision::Continue;
        path_buffer[0] = '/';
        __builtin_memcpy(path_buffer + 1, entry.path.characters_without_null_termination(), entry.path.length());
        StringView path { path_buffer, entry.path.length() + 1 };

        auto unpacked = [&]() -> ErrorOr<void> {
            TRY(make_parents(path, result));
            switch (entry.type) {
            case Archive::TarEntryType::Directory: {
                auto made = VirtualFileSystem::mkdir(path, "/"_sv, entry.mode);
                if (made.is_error() && made.error().code() != EEXIST)
                    return made.release_error();
                ++result.directories;
                return {};
            }
            case Archive::TarEntryType::SymbolicLink:
                return VirtualFileSystem::symlink(entry.link_target, path, "/"_sv);
            case Archive::TarEntryType::File: {
                auto file = TRY(VirtualFileSystem::open(path, "/"_sv, O_WRONLY | O_CREAT | O_TRUNC, entry.mode));
                TRY(file->write(IOBuffer::kernel(const_cast<u8*>(entry.data.data())), entry.data.size()));
                ++result.files;
                result.bytes += entry.data.size();
                return {};
            }
            case Archive::TarEntryType::Other:
                return {};
            }
            return {};
        }();
        if (unpacked.is_error()) {
            failure = unpacked.release_error();
            return Archive::IterationDecision::Break;
        }
        return Archive::IterationDecision::Continue;
    }));
    if (failure.has_value())
        return failure.release_value();
    return result;
}

}
