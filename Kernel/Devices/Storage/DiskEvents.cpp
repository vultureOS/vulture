/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/disk.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Storage/BlockDevice.h>
#include <Kernel/Devices/Storage/DiskEvents.h>
#include <Kernel/Devices/Storage/StorageControllers.h>
#include <Kernel/FileSystem/DevFS.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Tasks/Process.h>
#include <LibBase/IntrusiveList.h>
#include <LibBase/New.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

namespace Kernel::DiskEvents {

namespace {

class Stream;

Mutex s_lock;
WaitQueue s_waiters;
Vector<Stream*> s_streams;

class Stream final : public Device {
public:
    Stream()
        : Device(InodeType::CharacterDevice, 0444, device_number(7, 1))
    {
    }

    ~Stream() override
    {
        MutexLocker locker(s_lock);
        s_streams.remove_all_matching([this](Stream* stream) { return stream == this; });
    }

    ErrorOr<void> start()
    {
        MutexLocker locker(s_lock);
        // Whatever's already here, as if it had only now come.
        for (auto const& device : BlockDevice::all())
            TRY(add_line('+', device->name()));
        return s_streams.try_append(this);
    }

    ErrorOr<void> add_line(char kind, StringView name)
    {
        TRY(m_pending.try_append(kind));
        for (size_t i = 0; i < name.length(); ++i)
            TRY(m_pending.try_append(name[i]));
        return m_pending.try_append('\n');
    }

    [[nodiscard]] bool can_read() const override { return !m_pending.is_empty(); }

    ErrorOr<size_t> read(u64, size_t count, IOBuffer const& buffer) override
    {
        if (!s_waiters.wait_until_interruptible([this] { return can_read(); }))
            return Error::from_errno(EINTR);
        MutexLocker locker(s_lock);
        size_t const part = min(count, m_pending.size());
        TRY(buffer.write(0, { reinterpret_cast<u8 const*>(m_pending.begin()), part }));
        for (size_t i = part; i < m_pending.size(); ++i)
            m_pending[i - part] = m_pending[i];
        TRY(m_pending.try_resize(m_pending.size() - part));
        return part;
    }

private:
    Vector<char> m_pending;
};

class Control final : public Device {
public:
    Control()
        : Device(InodeType::CharacterDevice, 0444, device_number(7, 0))
    {
    }

    ErrorOr<NonnullRefPtr<Inode>> open_inode(int) override
    {
        auto* stream = new (Base::nothrow) Stream;
        if (!stream)
            return Error::from_errno(ENOMEM);
        auto reference = adopt_ref(*static_cast<Inode*>(stream));
        TRY(stream->start());
        return reference;
    }
};

// A file, as a disk.
class ImageDisk final : public BlockDevice {
public:
    ImageDisk(String name, NonnullRefPtr<Inode> file, u64 sectors, bool read_only)
        : BlockDevice(move(name), sectors, read_only)
        , m_file(move(file))
    {
    }

    ErrorOr<void> read_blocks(u64 first, size_t count, Bytes buffer) override
    {
        size_t const got = TRY(m_file->read(first * block_size, count * block_size, IOBuffer::kernel(buffer.data())));
        if (got != count * block_size)
            return Error::from_errno(EIO);
        return {};
    }

    ErrorOr<void> write_blocks(u64 first, size_t count, ReadonlyBytes buffer) override
    {
        if (is_read_only())
            return Error::from_errno(EROFS);
        size_t const wrote = TRY(m_file->write(first * block_size, count * block_size, IOBuffer::kernel(const_cast<u8*>(buffer.data()))));
        if (wrote != count * block_size)
            return Error::from_errno(EIO);
        return {};
    }

    ErrorOr<void> flush() override { return m_file->sync(); }
    [[nodiscard]] bool is_removable() const override { return true; }
    [[nodiscard]] StringView bus_name() const override { return "disk image"_sv; }

    ErrorOr<void> eject() override
    {
        BlockDevice::remove_disk(*this);
        return {};
    }

private:
    NonnullRefPtr<Inode> m_file;
};

class Images final : public Device {
public:
    Images()
        : Device(InodeType::CharacterDevice, 0444, device_number(7, 2))
    {
    }

    ErrorOr<FlatPtr> ioctl(FileDescription&, unsigned request, FlatPtr argument) override
    {
        if (request != VULTURE_DISKIMAGE_ATTACH && request != VULTURE_DISKIMAGE_ATTACH_READ_ONLY)
            return Error::from_errno(ENOTTY);
        auto* process = Process::current();
        if (!process->credentials().is_superuser())
            return Error::from_errno(EPERM);
        int const fd = TRY(copy_from_user<int>(argument));
        auto description = TRY(process->file(fd));
        if (!description->inode())
            return Error::from_errno(EBADF);
        auto file = NonnullRefPtr<Inode>(*description->inode());
        auto const metadata = file->metadata();
        if (metadata.type != InodeType::File)
            return Error::from_errno(EINVAL);
        bool const read_only = request == VULTURE_DISKIMAGE_ATTACH_READ_ONLY || !description->is_writable();
        u32 const number = take_disk_number();
        StringBuilder name;
        name.appendff("disk{}", number);
        auto* disk = new (Base::nothrow) ImageDisk(TRY(name.to_string()), move(file), metadata.size / BlockDevice::block_size, read_only);
        if (!disk)
            return Error::from_errno(ENOMEM);
        auto reference = adopt_ref(*static_cast<BlockDevice*>(disk));
        add_disk(reference, "disk image"_sv);
        return number;
    }
};

void tell_everyone(char kind, StringView name)
{
    {
        MutexLocker locker(s_lock);
        for (auto* stream : s_streams)
            (void)stream->add_line(kind, name);
    }
    s_waiters.wake_all();
    notify_pollers();
}

}

ErrorOr<void> create_devices()
{
    auto* control = new (Base::nothrow) Control;
    if (!control)
        return Error::from_errno(ENOMEM);
    TRY(DevFS::the().add_device("diskevents"_sv, adopt_ref(*static_cast<Inode*>(control))));
    auto* images = new (Base::nothrow) Images;
    if (!images)
        return Error::from_errno(ENOMEM);
    return DevFS::the().add_device("diskimages"_sv, adopt_ref(*static_cast<Inode*>(images)));
}

void disk_appeared(StringView name)
{
    tell_everyone('+', name);
}

void disk_disappeared(StringView name)
{
    tell_everyone('-', name);
}

}
