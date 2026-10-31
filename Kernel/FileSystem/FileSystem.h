/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/Inode.h>

namespace Kernel {

// One mounted filesystem: tmpfs, devfs, the boot archive, FAT, vultureFS.
class FileSystem : public RefCounted<FileSystem> {
public:
    virtual ~FileSystem() = default;

    [[nodiscard]] virtual StringView name() const = 0;
    [[nodiscard]] virtual NonnullRefPtr<Inode> root_inode() = 0;
    [[nodiscard]] virtual bool is_read_only() const { return false; }
    // Writes everything to stable storage.
    virtual ErrorOr<void> sync() { return {}; }

    // Moves a name within this filesystem, replacing an existing target.
    virtual ErrorOr<void> rename(Inode& old_parent, StringView old_name, Inode& new_parent, StringView new_name);

    [[nodiscard]] u32 id() const { return m_id; }

protected:
    FileSystem();

private:
    u32 m_id;
};

}
