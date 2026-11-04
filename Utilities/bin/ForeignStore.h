/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibForeignFS/Volume.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

// A disk's device or an image file, as what LibForeignFS works on.
class ForeignStore final : public ForeignFS::Store {
public:
    static ErrorOr<NonnullOwnPtr<ForeignStore>> open_path(char const* path, bool read_only)
    {
        int const fd = open(path, read_only ? O_RDONLY : O_RDWR);
        if (fd < 0)
            return Error::from_errno(errno);
        struct stat information;
        if (fstat(fd, &information) != 0) {
            int const problem = errno;
            close(fd);
            return Error::from_errno(problem);
        }
        u64 const sectors = static_cast<u64>(information.st_size) / sector_size;
        auto* store = new (Base::nothrow) ForeignStore(fd, sectors, read_only);
        if (!store) {
            close(fd);
            return Error::from_errno(ENOMEM);
        }
        return adopt_own(*store);
    }

    ~ForeignStore() override { close(m_fd); }

    [[nodiscard]] u64 sector_count() const override { return m_sectors; }
    [[nodiscard]] bool is_read_only() const override { return m_read_only; }
    ErrorOr<void> read_sectors(u64 first, size_t count, Bytes buffer) override
    {
        ssize_t const got = pread(m_fd, buffer.data(), count * sector_size, static_cast<off_t>(first * sector_size));
        if (got != static_cast<ssize_t>(count * sector_size))
            return Error::from_errno(got < 0 ? errno : EIO);
        return {};
    }
    ErrorOr<void> write_sectors(u64 first, size_t count, ReadonlyBytes buffer) override
    {
        ssize_t const wrote = pwrite(m_fd, buffer.data(), count * sector_size, static_cast<off_t>(first * sector_size));
        if (wrote != static_cast<ssize_t>(count * sector_size))
            return Error::from_errno(wrote < 0 ? errno : EIO);
        return {};
    }
    ErrorOr<void> flush() override
    {
        fsync(m_fd);
        return {};
    }

private:
    ForeignStore(int fd, u64 sectors, bool read_only)
        : m_fd(fd)
        , m_sectors(sectors)
        , m_read_only(read_only)
    {
    }

    int m_fd;
    u64 m_sectors;
    bool m_read_only;
};
