/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main()
{
    char buffer[64];
    int null = open("/dev/null", O_RDWR);
    CHECK(null >= 0);
    CHECK(write(null, "gone", 4) == 4);
    CHECK(read(null, buffer, sizeof(buffer)) == 0);
    close(null);

    int zero = open("/dev/zero", O_RDONLY);
    memset(buffer, 0xff, sizeof(buffer));
    CHECK(read(zero, buffer, sizeof(buffer)) == static_cast<ssize_t>(sizeof(buffer)));
    bool all_zero = true;
    for (char c : buffer)
        all_zero = all_zero && c == 0;
    CHECK(all_zero);
    close(zero);

    int random = open("/dev/urandom", O_RDONLY);
    unsigned char first[32] {}, second[32] {};
    CHECK(read(random, first, sizeof(first)) == 32 && read(random, second, sizeof(second)) == 32);
    CHECK(memcmp(first, second, sizeof(first)) != 0);
    close(random);

    struct stat information {};
    CHECK(stat("/dev/console", &information) == 0 && S_ISCHR(information.st_mode));
    CHECK(isatty(STDOUT_FILENO) && !isatty(-1));
    return finish();
}
