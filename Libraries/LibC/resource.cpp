/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {

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

}
