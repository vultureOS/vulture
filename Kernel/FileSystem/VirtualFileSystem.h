/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/FileSystem.h>
#include <LibBase/String.h>

namespace Kernel {

class FileDescription;

struct ResolvedPath {
    NonnullRefPtr<Inode> inode;
    // The canonical absolute path: no ".", "..", symbolic links or "//".
    String path;
};

struct ResolvedParent {
    NonnullRefPtr<Inode> directory;
    String directory_path;
    String name; // The last component, which may not exist yet.
};

// The single tree of every mounted filesystem. Paths are resolved one
// component at a time on a stack of the inodes walked through, so ".."
// goes back the way it came (across mount points too), and symbolic links
// are spliced into the rest of the path.
class VirtualFileSystem {
public:
    static constexpr size_t max_path = 1024;
    static constexpr size_t max_symbolic_links = 40;

    static void mount_root(NonnullRefPtr<FileSystem>);
    static ErrorOr<void> mount(NonnullRefPtr<FileSystem>, StringView path);
    [[nodiscard]] static bool has_root();

    // `cwd` is the canonical working directory paths start from.
    static ErrorOr<ResolvedPath> resolve(StringView path, StringView cwd, bool follow_last_link = true);
    static ErrorOr<ResolvedParent> resolve_parent(StringView path, StringView cwd);

    static ErrorOr<NonnullRefPtr<FileDescription>> open(StringView path, StringView cwd, int flags, u32 permissions);
    static ErrorOr<void> mkdir(StringView path, StringView cwd, u32 permissions);
    // Something with a name that isn't a file or a directory: a named pipe, a socket.
    static ErrorOr<void> make_node(StringView path, StringView cwd, InodeType, u32 permissions);
    // Whichever of them is mounted here, gone. EBUSY if `in_use` says someone's still using it.
    using InUse = bool (*)(FileSystem&, StringView path);
    static ErrorOr<void> unmount(StringView path, StringView cwd, InUse in_use);
    static ErrorOr<void> unlink(StringView path, StringView cwd);
    static ErrorOr<void> rmdir(StringView path, StringView cwd);
    static ErrorOr<void> rename(StringView old_path, StringView new_path, StringView cwd);
    static ErrorOr<void> symlink(StringView target, StringView link_path, StringView cwd);
    static ErrorOr<void> link(StringView existing_path, StringView new_path, StringView cwd);
    static ErrorOr<void> sync_all();
    // Memory's short: what's only being kept to save reading it again goes, up to this many pages from all the filesystems. How many.
    static size_t release_file_cache(size_t pages);
    // How many pages of files are in memory.
    [[nodiscard]] static size_t file_cache_pages();
    // Each mounted filesystem with where it is mounted.
    using Function = ErrorOr<void> (*)(void* context, FileSystem&, StringView path);
    static ErrorOr<void> for_each_mount_point(Function, void* context);

    template<typename Callback>
    static void for_each_mount(Callback callback);
};

}
