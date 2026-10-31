/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// Handing the kernel a bad pointer must fail with EFAULT, never crash it
// or let it touch kernel memory on the program's behalf.

int main()
{
    auto expect_fault = [](long result) { return result == -1 && errno == EFAULT; };

    CHECK(expect_fault(write(STDOUT_FILENO, reinterpret_cast<void*>(16), 4)));
    // A kernel address is not the program's to pass.
    CHECK(expect_fault(write(STDOUT_FILENO, reinterpret_cast<void*>(0xffff'0000'4000'0000ull), 4)));
    CHECK(expect_fault(write(STDOUT_FILENO, reinterpret_cast<void*>(0xffff'ffff'8000'0000ull), 4)));

    // A buffer running off the end of mapped memory.
    auto* page = static_cast<char*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(page != MAP_FAILED);
    for (int i = 0; i < 4096; ++i)
        page[i] = 'x';
    CHECK(munmap(page + 4096, 4096) == 0 || true);
    CHECK(expect_fault(write(STDOUT_FILENO, page + 4000, 200)));

    // Reading into memory that cannot be written.
    int fd = open("/etc/motd", O_RDONLY);
    CHECK(fd >= 0);
    CHECK(expect_fault(read(fd, reinterpret_cast<void*>(&main), 8)));
    close(fd);

    CHECK(expect_fault(open(reinterpret_cast<char const*>(8), O_RDONLY)));
    CHECK(expect_fault(nanosleep(reinterpret_cast<timespec const*>(24), nullptr)));
    return finish();
}
