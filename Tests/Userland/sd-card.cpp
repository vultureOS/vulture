/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

// A disk that stays, on whatever the system started from: Meta/raspberry-pi-test.sh runs this twice, with a restart
// between. The first time it writes something bigger than the most a card takes in one go, and the second
// time, with no cache left to answer, it should still be there.

namespace {

constexpr char const* path = "/Users/Shared/sd-card-test";
constexpr size_t size = 2 * 1024 * 1024;
unsigned char s_written[size];
unsigned char s_read[size];

}

int main()
{
    for (size_t i = 0; i < size; ++i)
        s_written[i] = static_cast<unsigned char>(i * 31 + i / 512);

    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        CHECK(read(fd, s_read, size) == static_cast<ssize_t>(size));
        close(fd);
        CHECK(memcmp(s_written, s_read, size) == 0);
        if (g_failures == 0)
            printf("sd-card: read back what was written before the restart\n");
        return finish();
    }

    fd = open(path, O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, s_written, size) == static_cast<ssize_t>(size));
    CHECK(fsync(fd) == 0);
    close(fd);
    if (g_failures == 0)
        printf("sd-card: wrote it\n");
    return finish();
}
