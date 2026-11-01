/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

// The kernel's limits; a process may lower the soft ones.
rlimit s_limits[RLIM_NLIMITS] = {
    { RLIM_INFINITY, RLIM_INFINITY }, // CPU
    { RLIM_INFINITY, RLIM_INFINITY }, // FSIZE
    { RLIM_INFINITY, RLIM_INFINITY }, // DATA
    { 8 * 1024 * 1024, 8 * 1024 * 1024 }, // STACK
    { 0, 0 }, // CORE: no core files are written.
    { RLIM_INFINITY, RLIM_INFINITY }, // AS
    { RLIM_INFINITY, RLIM_INFINITY }, // MEMLOCK
    { RLIM_INFINITY, RLIM_INFINITY }, // NPROC
    { 64, 64 }, // NOFILE
};

}

extern "C" {

int getrlimit(int resource, struct rlimit* limit)
{
    if (resource < 0 || resource >= RLIM_NLIMITS || !limit) {
        errno = EINVAL;
        return -1;
    }
    *limit = s_limits[resource];
    return 0;
}

int setrlimit(int resource, struct rlimit const* limit)
{
    if (resource < 0 || resource >= RLIM_NLIMITS || !limit || limit->rlim_cur > limit->rlim_max) {
        errno = EINVAL;
        return -1;
    }
    // Raising the hard limit takes root; past the kernel's, nobody.
    if (limit->rlim_max > s_limits[resource].rlim_max && geteuid() != 0) {
        errno = EPERM;
        return -1;
    }
    if (limit->rlim_max > s_limits[resource].rlim_max) {
        errno = EINVAL;
        return -1;
    }
    s_limits[resource] = *limit;
    return 0;
}

int msync(void* address, size_t, int flags)
{
    if ((reinterpret_cast<unsigned long>(address) & 4095) || ((flags & MS_SYNC) && (flags & MS_ASYNC))) {
        errno = EINVAL;
        return -1;
    }
    if (flags & MS_SYNC)
        sync();
    return 0;
}

int madvise(void* address, size_t, int advice)
{
    if ((reinterpret_cast<unsigned long>(address) & 4095) || advice < MADV_NORMAL || advice > MADV_DONTNEED) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int posix_madvise(void* address, size_t length, int advice)
{
    return madvise(address, length, advice) < 0 ? errno : 0;
}

}
