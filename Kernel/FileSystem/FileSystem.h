/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/Inode.h>
#include <LibBase/Function.h>

namespace Kernel {

// One mounted filesystem: tmpfs, devfs, the boot archive, FAT, vultureFS.
class FileSystem : public RefCounted<FileSystem> {
public:
    virtual ~FileSystem() = default;

    [[nodiscard]] virtual StringView name() const = 0;
    [[nodiscard]] virtual NonnullRefPtr<Inode> root_inode() = 0;
    [[nodiscard]] virtual bool is_read_only() const { return false; }

    struct Statistics {
        u64 block_size { 4096 };
        u64 blocks { 0 };
        u64 free_blocks { 0 };
    };
    [[nodiscard]] virtual Statistics statistics() { return {}; }
    // The device it is on ("disk1s1"), or its type for those on none.
    [[nodiscard]] virtual StringView device_name() const { return name(); }
    [[nodiscard]] virtual bool device_has_gone() const { return false; }
    // Writes everything to stable storage.
    virtual ErrorOr<void> sync() { return {}; }
    // Memory's short: gives back up to this many pages of what it's only keeping to save reading it again. How many it did.
    virtual size_t release_cached_pages(size_t) { return 0; }
    [[nodiscard]] virtual size_t cached_page_count() const { return 0; }

    // A copy that has the same blocks as the original until one of them changes.
    virtual ErrorOr<NonnullRefPtr<Inode>> clone(Inode& source, Inode& directory, StringView name, u32 uid, u32 gid);

    // This moment of it, kept. Going back to one takes effect at the next mount.
    virtual ErrorOr<void> create_snapshot(StringView name);
    virtual ErrorOr<void> delete_snapshot(StringView name);
    virtual ErrorOr<void> revert_to_snapshot(StringView name);
    virtual ErrorOr<Vector<String>> snapshots();

    // What's changed, where it's kept: see FSEvents.
    virtual ErrorOr<u64> append_event(u32 flags, StringView path);
    [[nodiscard]] virtual u64 next_event() { return 0; }
    [[nodiscard]] virtual u64 oldest_event() { return 0; }
    using EventVisitor = Function<bool(u64 id, u32 flags, StringView path)>;
    virtual ErrorOr<void> for_each_event(u64 first, EventVisitor);

    // Moves a name within this filesystem, replacing an existing target.
    virtual ErrorOr<void> rename(Inode& old_parent, StringView old_name, Inode& new_parent, StringView new_name);

    [[nodiscard]] u32 id() const { return m_id; }

protected:
    FileSystem();

private:
    u32 m_id;
};

}
