/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/FileSystem/BootArchive.h>
#include <LibArchive/Tar.h>

namespace Kernel {

static Optional<Archive::TarReader> s_reader;
static ReadonlyBytes s_archive;
static size_t s_entry_count = 0;

ErrorOr<void> BootArchive::initialize(ReadonlyBytes archive)
{
    auto reader = TRY(Archive::TarReader::parse(archive));
    s_entry_count = 0;
    TRY(reader.for_each_entry([](Archive::TarEntry const&) {
        ++s_entry_count;
        return Archive::IterationDecision::Continue;
    }));
    s_archive = archive;
    s_reader = reader;
    return {};
}

bool BootArchive::is_available()
{
    return s_reader.has_value();
}

size_t BootArchive::entry_count()
{
    return s_entry_count;
}

Optional<BootArchive::Node> BootArchive::lookup(StringView path)
{
    if (!s_reader.has_value())
        return {};
    auto relative = Archive::TarReader::normalize(path);
    if (relative.is_empty())
        return Node { {}, true, 0755, 1 };
    if (auto entry = s_reader->find(relative); entry.has_value()) {
        Node node;
        node.data = entry->data;
        node.is_directory = entry->type == Archive::TarEntryType::Directory;
        node.mode = entry->mode & 07777;
        // The entry's place in the archive is a stable, unique number.
        node.inode = static_cast<u64>(entry->data.data() - s_archive.data()) / 512 + 2;
        return node;
    }
    // A directory may exist only implicitly, as the prefix of other paths.
    bool is_prefix = false;
    (void)s_reader->for_each_entry([&](Archive::TarEntry const& entry) {
        if (entry.path.length() > relative.length() && entry.path.starts_with(relative) && entry.path[relative.length()] == '/') {
            is_prefix = true;
            return Archive::IterationDecision::Break;
        }
        return Archive::IterationDecision::Continue;
    });
    if (is_prefix)
        return Node { {}, true, 0755, 0 };
    return {};
}

}
