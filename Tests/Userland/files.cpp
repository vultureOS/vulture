/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main()
{
    char const expected[] = "Welcome to vultureOS.\n";
    int fd = open("/etc/motd", O_RDONLY);
    CHECK(fd >= 3);
    char buffer[64] {};
    CHECK(read(fd, buffer, sizeof(buffer)) == static_cast<ssize_t>(strlen(expected)));
    CHECK(strcmp(buffer, expected) == 0);
    CHECK(read(fd, buffer, sizeof(buffer)) == 0);

    struct stat information {};
    CHECK(fstat(fd, &information) == 0);
    CHECK(information.st_size == static_cast<off_t>(strlen(expected)));
    CHECK(S_ISREG(information.st_mode));

    CHECK(lseek(fd, 11, SEEK_SET) == 11);
    memset(buffer, 0, sizeof(buffer));
    CHECK(read(fd, buffer, 9) == 9);
    CHECK(strcmp(buffer, "vultureOS") == 0);
    CHECK(lseek(fd, -1, SEEK_END) == static_cast<off_t>(strlen(expected)) - 1);

    CHECK(write(fd, "x", 1) == -1 && errno == EBADF);
    CHECK(close(fd) == 0);
    CHECK(close(fd) == -1 && errno == EBADF);
    CHECK(read(fd, buffer, 1) == -1 && errno == EBADF);

    CHECK(open("/etc/nonexistent", O_RDONLY) == -1 && errno == ENOENT);
    CHECK(open("/etc/motd", O_WRONLY) == -1 && errno == EROFS);

    CHECK(stat("/sbin/init", &information) == 0);
    CHECK(S_ISREG(information.st_mode) && (information.st_mode & 0111));
    CHECK(stat("/etc", &information) == 0 && S_ISDIR(information.st_mode));
    CHECK(stat("/", &information) == 0 && S_ISDIR(information.st_mode));

    // The standard streams are open.
    CHECK(write(STDOUT_FILENO, "", 0) == 0);
    CHECK(fstat(STDOUT_FILENO, &information) == 0 && S_ISCHR(information.st_mode));
    return finish();
}
