/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/FileSystem/DevFS.h>
#include <Kernel/Time/Time.h>
#include <LibBase/FastRandom.h>
#include <LibBase/New.h>

namespace Kernel {

static DevFS* s_the;

class DevFSRoot final : public Inode {
public:
    explicit DevFSRoot(DevFS& file_system)
        : Inode(file_system, 1)
    {
    }

    [[nodiscard]] InodeMetadata metadata() const override
    {
        InodeMetadata metadata;
        metadata.index = 1;
        metadata.type = InodeType::Directory;
        metadata.permissions = 0755;
        metadata.link_count = 2;
        return metadata;
    }

    ErrorOr<NonnullRefPtr<Inode>> lookup(StringView name) override
    {
        if (name == "."_sv || name == ".."_sv)
            return NonnullRefPtr<Inode>(*this);
        MutexLocker locker(m_lock);
        for (auto& entry : m_entries) {
            if (entry.name == name)
                return entry.inode;
        }
        return Error::from_errno(ENOENT);
    }

    ErrorOr<void> traverse(DirectoryVisitor& visitor) override
    {
        if (visitor.visit("."_sv, 1, InodeType::Directory) == VisitDecision::Stop || visitor.visit(".."_sv, 1, InodeType::Directory) == VisitDecision::Stop)
            return {};
        MutexLocker locker(m_lock);
        for (auto& entry : m_entries) {
            if (visitor.visit(entry.name.view(), entry.inode->index(), entry.inode->metadata().type) == VisitDecision::Stop)
                break;
        }
        return {};
    }

    ErrorOr<void> add(StringView name, NonnullRefPtr<Inode> inode)
    {
        MutexLocker locker(m_lock);
        return m_entries.try_append(Entry { TRY(String::from_view(name)), move(inode) });
    }

private:
    struct Entry {
        String name;
        NonnullRefPtr<Inode> inode;
    };

    Mutex m_lock;
    Vector<Entry> m_entries;
};

ErrorOr<NonnullRefPtr<DevFS>> DevFS::create()
{
    auto* file_system = new (Base::nothrow) DevFS;
    if (!file_system)
        return Error::from_errno(ENOMEM);
    auto reference = adopt_ref(*file_system);
    auto* root = new (Base::nothrow) DevFSRoot(*reference);
    if (!root)
        return Error::from_errno(ENOMEM);
    reference->m_root = adopt_ref(*root);
    s_the = reference.ptr();
    return reference;
}

DevFS& DevFS::the()
{
    VERIFY(s_the);
    return *s_the;
}

NonnullRefPtr<Inode> DevFS::root_inode()
{
    return *m_root;
}

ErrorOr<void> DevFS::add_device(StringView name, NonnullRefPtr<Inode> inode)
{
    return m_root->add(name, move(inode));
}

Device::Device(InodeType type, u32 permissions, u64 device_number)
    : Inode(DevFS::the(), DevFS::the().allocate_index())
    , m_type(type)
    , m_permissions(permissions)
    , m_device_number(device_number)
{
}

InodeMetadata Device::metadata() const
{
    InodeMetadata metadata;
    metadata.index = index();
    metadata.type = m_type;
    metadata.permissions = m_permissions;
    metadata.device = m_device_number;
    return metadata;
}

namespace {

constexpr u64 device_number(u32 major, u32 minor)
{
    return (static_cast<u64>(major) << 8) | minor;
}

class NullDevice final : public Device {
public:
    NullDevice()
        : Device(InodeType::CharacterDevice, 0666, device_number(1, 3))
    {
    }
    ErrorOr<size_t> read(u64, size_t, IOBuffer const&) override { return 0; }
    ErrorOr<size_t> write(u64, size_t count, IOBuffer const&) override { return count; }
};

class ZeroDevice final : public Device {
public:
    ZeroDevice()
        : Device(InodeType::CharacterDevice, 0666, device_number(1, 5))
    {
    }
    ErrorOr<size_t> read(u64, size_t count, IOBuffer const& buffer) override
    {
        TRY(buffer.fill_with_zeros(0, count));
        return count;
    }
    ErrorOr<size_t> write(u64, size_t count, IOBuffer const&) override { return count; }
};

class RandomDevice final : public Device {
public:
    RandomDevice()
        : Device(InodeType::CharacterDevice, 0666, device_number(1, 9))
        , m_random(Time::now() ^ 0x5eed'cafe'f00d'd00dull)
    {
    }
    ErrorOr<size_t> read(u64, size_t count, IOBuffer const& buffer) override
    {
        size_t done = 0;
        while (done < count) {
            u64 word;
            {
                MutexLocker locker(m_lock);
                word = m_random.next();
            }
            size_t chunk = min(count - done, sizeof(word));
            TRY(buffer.write(done, { reinterpret_cast<u8 const*>(&word), chunk }));
            done += chunk;
        }
        return done;
    }
    ErrorOr<size_t> write(u64, size_t count, IOBuffer const&) override { return count; }

private:
    Mutex m_lock;
    FastRandom m_random;
};

class ConsoleDevice final : public Device {
public:
    ConsoleDevice()
        : Device(InodeType::CharacterDevice, 0620, device_number(5, 1))
    {
    }
    // No keyboard yet (M6): end of file.
    ErrorOr<size_t> read(u64, size_t, IOBuffer const&) override { return 0; }
    ErrorOr<size_t> write(u64, size_t count, IOBuffer const& buffer) override
    {
        char chunk[256];
        size_t done = 0;
        while (done < count) {
            size_t length = min(count - done, sizeof(chunk));
            auto result = buffer.read(done, { reinterpret_cast<u8*>(chunk), length });
            if (result.is_error())
                return done > 0 ? ErrorOr<size_t>(done) : ErrorOr<size_t>(result.release_error());
            kernel_log().write_raw({ chunk, length });
            done += length;
        }
        return done;
    }
};

template<typename T>
ErrorOr<void> add(StringView name)
{
    auto* device = new (Base::nothrow) T;
    if (!device)
        return Error::from_errno(ENOMEM);
    return DevFS::the().add_device(name, adopt_ref(*static_cast<Inode*>(device)));
}

}

ErrorOr<void> create_basic_devices()
{
    TRY(add<NullDevice>("null"_sv));
    TRY(add<ZeroDevice>("zero"_sv));
    TRY(add<RandomDevice>("random"_sv));
    TRY(add<RandomDevice>("urandom"_sv));
    TRY(add<ConsoleDevice>("console"_sv));
    return {};
}

}
