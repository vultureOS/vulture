/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/files.h>
#include <Kernel/FileSystem/BootArchive.h>
#include <Kernel/Locking/SpinLock.h>
#include <LibBase/RefPtr.h>

namespace Kernel {

// What a file descriptor refers to: the console, or a file or directory of
// the boot archive. Shared by descriptors that were duplicated (fork), which
// then share the offset, as POSIX says.
class OpenFile final : public RefCounted<OpenFile> {
public:
    enum class Kind : u8 {
        Console,
        ArchiveFile,
        ArchiveDirectory,
    };

    [[nodiscard]] static ErrorOr<NonnullRefPtr<OpenFile>> open_console();
    [[nodiscard]] static ErrorOr<NonnullRefPtr<OpenFile>> open_path(StringView path, int flags);

    ErrorOr<size_t> read(FlatPtr user_buffer, size_t size);
    ErrorOr<size_t> write(FlatPtr user_buffer, size_t size);
    ErrorOr<i64> seek(i64 offset, int origin);
    [[nodiscard]] vulture_stat stat() const;

    [[nodiscard]] Kind kind() const { return m_kind; }
    [[nodiscard]] bool close_on_exec() const { return m_close_on_exec; }

    static vulture_stat stat_for(BootArchive::Node const&);

private:
    OpenFile(Kind, int flags);

    Kind m_kind;
    int m_flags;
    bool m_close_on_exec { false };
    BootArchive::Node m_node;
    SpinLock m_lock;
    u64 m_offset { 0 };
};

}
