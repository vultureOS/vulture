/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/FileSystem.h>
#include <Kernel/Locking/Mutex.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

namespace Kernel {

class DevFSRoot;

// /dev: one flat directory of device nodes, which drivers add as they find
// their devices.
class DevFS final : public FileSystem {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<DevFS>> create();
    [[nodiscard]] static DevFS& the();

    [[nodiscard]] StringView name() const override { return "devfs"_sv; }
    [[nodiscard]] NonnullRefPtr<Inode> root_inode() override;
    [[nodiscard]] bool is_read_only() const override { return true; }

    ErrorOr<void> add_device(StringView name, NonnullRefPtr<Inode>);
    void remove_device(StringView name);
    [[nodiscard]] InodeIndex allocate_index() { return m_next_index.fetch_add(1); }

private:
    DevFS() = default;

    RefPtr<DevFSRoot> m_root;
    Atomic<InodeIndex> m_next_index { 2 };
};

// Device numbers, major and minor, as stat() shows them.
constexpr u64 device_number(u32 major, u32 minor)
{
    return (static_cast<u64>(major) << 8) | minor;
}

// A device node: reads and writes go straight to a driver.
class Device : public Inode {
public:
    [[nodiscard]] InodeMetadata metadata() const override;
    ErrorOr<void> set_owner(u32 uid, u32 gid) override
    {
        m_uid = uid;
        m_gid = gid;
        return {};
    }

protected:
    Device(InodeType, u32 permissions, u64 device_number);

private:
    InodeType m_type;
    u32 m_permissions;
    u64 m_device_number;
    u32 m_uid { 0 };
    u32 m_gid { 0 };
};

// The memory devices (null, zero, random: pseudo-random, not for keys yet)
// and /dev/tty. The console and pseudo-terminals come from Kernel/TTY.
ErrorOr<void> create_basic_devices();

}
