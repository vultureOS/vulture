/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The VFS on the data volume (/tmp): files, directories, links, renames, paths.

static bool write_file(char const* path, char const* text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    bool ok = write(fd, text, strlen(text)) == static_cast<ssize_t>(strlen(text));
    return close(fd) == 0 && ok;
}

static bool file_holds(char const* path, char const* text)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    char buffer[256] {};
    ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    return length == static_cast<ssize_t>(strlen(text)) && strcmp(buffer, text) == 0;
}

static int count_entries(char const* path)
{
    DIR* directory = opendir(path);
    if (!directory)
        return -1;
    int count = 0;
    while (auto* entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            ++count;
    }
    closedir(directory);
    return count;
}

int main()
{
    CHECK(mkdir("/tmp/test", 0755) == 0);
    CHECK(mkdir("/tmp/test", 0755) == -1 && errno == EEXIST);
    CHECK(write_file("/tmp/test/a.txt", "alpha"));
    CHECK(file_holds("/tmp/test/a.txt", "alpha"));

    // Append, seek past the end (a hole reads as zeros), truncate.
    int fd = open("/tmp/test/a.txt", O_WRONLY | O_APPEND);
    CHECK(fd >= 0 && write(fd, "-beta", 5) == 5);
    close(fd);
    CHECK(file_holds("/tmp/test/a.txt", "alpha-beta"));
    fd = open("/tmp/test/sparse", O_RDWR | O_CREAT, 0600);
    CHECK(lseek(fd, 100'000, SEEK_SET) == 100'000 && write(fd, "!", 1) == 1);
    struct stat information {};
    CHECK(fstat(fd, &information) == 0 && information.st_size == 100'001);
    char byte = 'x';
    CHECK(lseek(fd, 50'000, SEEK_SET) == 50'000 && read(fd, &byte, 1) == 1 && byte == 0);
    CHECK(ftruncate(fd, 10) == 0 && fstat(fd, &information) == 0 && information.st_size == 10);
    close(fd);

    // A file bigger than a page, written and read back in odd pieces.
    constexpr size_t large = 200'000;
    auto* pattern = static_cast<unsigned char*>(malloc(large));
    for (size_t i = 0; i < large; ++i)
        pattern[i] = static_cast<unsigned char>(i * 13 + 7);
    fd = open("/tmp/test/large", O_RDWR | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0 && write(fd, pattern, large) == static_cast<ssize_t>(large));
    CHECK(open("/tmp/test/large", O_RDWR | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST);
    auto* back = static_cast<unsigned char*>(malloc(large));
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    size_t got = 0;
    while (got < large) {
        ssize_t chunk = read(fd, back + got, 7777);
        if (chunk <= 0)
            break;
        got += static_cast<size_t>(chunk);
    }
    CHECK(got == large && memcmp(pattern, back, large) == 0);
    close(fd);
    free(pattern);
    free(back);

    // Working directory and relative paths, with ".." along the way.
    CHECK(chdir("/tmp/test") == 0);
    char cwd[256];
    CHECK(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/private/tmp/test") == 0);
    CHECK(file_holds("a.txt", "alpha-beta"));
    CHECK(mkdir("sub", 0755) == 0 && chdir("sub/../sub/.") == 0);
    CHECK(getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/private/tmp/test/sub") == 0);
    CHECK(file_holds("../a.txt", "alpha-beta"));
    CHECK(chdir("/") == 0);

    // Symbolic links, relative and absolute, and a loop.
    CHECK(symlink("a.txt", "/tmp/test/link") == 0);
    CHECK(file_holds("/tmp/test/link", "alpha-beta"));
    char target[64] {};
    CHECK(readlink("/tmp/test/link", target, sizeof(target)) == 5 && strcmp(target, "a.txt") == 0);
    CHECK(lstat("/tmp/test/link", &information) == 0 && S_ISLNK(information.st_mode));
    CHECK(stat("/tmp/test/link", &information) == 0 && S_ISREG(information.st_mode));
    CHECK(symlink("/tmp/test", "/tmp/dirlink") == 0 && file_holds("/tmp/dirlink/a.txt", "alpha-beta"));
    CHECK(symlink("loop", "/tmp/test/loop") == 0);
    CHECK(open("/tmp/test/loop", O_RDONLY) == -1 && errno == ELOOP);

    // Renames: a file, onto an existing file, and a directory.
    CHECK(rename("/tmp/test/a.txt", "/tmp/test/b.txt") == 0);
    CHECK(access("/tmp/test/a.txt", F_OK) == -1 && errno == ENOENT && file_holds("/tmp/test/b.txt", "alpha-beta"));
    CHECK(write_file("/tmp/test/c.txt", "gamma") && rename("/tmp/test/c.txt", "/tmp/test/b.txt") == 0);
    CHECK(file_holds("/tmp/test/b.txt", "gamma"));
    CHECK(rename("/tmp/test/sub", "/tmp/moved") == 0 && stat("/tmp/moved", &information) == 0 && S_ISDIR(information.st_mode));
    CHECK(rename("/tmp/moved", "/tmp/moved/inside") == -1 && errno == EINVAL);

    // Listing, then removal.
    CHECK(count_entries("/tmp/test") == 5); // b.txt sparse large link loop
    CHECK(rmdir("/tmp/test") == -1 && errno == ENOTEMPTY);
    CHECK(unlink("/tmp/test") == -1 && errno == EISDIR);
    CHECK(rmdir("/tmp/test/b.txt") == -1 && errno == ENOTDIR);
    char const* const names[] = { "b.txt", "sparse", "large", "link", "loop" };
    for (char const* name : names) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/test/%s", name);
        CHECK(unlink(path) == 0);
    }
    CHECK(count_entries("/tmp/test") == 0);
    CHECK(rmdir("/tmp/test") == 0 && rmdir("/tmp/moved") == 0 && unlink("/tmp/dirlink") == 0);
    CHECK(access("/tmp/test", F_OK) == -1 && errno == ENOENT);

    // The root is writable too, and /dev is a mount point there.
    CHECK(count_entries("/dev") >= 5);
    CHECK(rmdir("/dev") == -1 && errno == EBUSY);
    return finish();
}
