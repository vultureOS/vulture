/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr size_t page = 4096;

char byte_at(int fd, off_t offset)
{
    char byte = 0;
    CHECK(lseek(fd, offset, SEEK_SET) == offset);
    CHECK(read(fd, &byte, 1) == 1);
    return byte;
}

// Files mapped into memory share their pages with read() and write().
void test_mapping(char const* path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    char contents[page];
    for (int i = 0; i < 3; ++i) {
        memset(contents, 'a' + i, sizeof(contents));
        CHECK(write(fd, contents, page) == static_cast<ssize_t>(page));
    }

    // Private: the file's contents; writes stay in this process.
    auto* mine = static_cast<char*>(mmap(nullptr, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0));
    CHECK(mine != MAP_FAILED);
    CHECK(mine[0] == 'a' && mine[page] == 'b' && mine[3 * page - 1] == 'c');
    mine[10] = 'X';
    CHECK(mine[10] == 'X' && byte_at(fd, 10) == 'a');

    // Shared, from the second page: one copy, seen from both sides.
    auto* shared = static_cast<char*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page));
    CHECK(shared != MAP_FAILED);
    CHECK(shared[0] == 'b' && shared[page] == 'c');
    shared[1] = 'Y';
    CHECK(byte_at(fd, page + 1) == 'Y');
    CHECK(lseek(fd, page + 5, SEEK_SET) == static_cast<off_t>(page + 5) && write(fd, "Z", 1) == 1);
    CHECK(shared[5] == 'Z');

    // A child shares the mapping across fork.
    pid_t child = fork();
    if (child == 0) {
        shared[2] = 'W';
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
    CHECK(shared[2] == 'W');

    CHECK(munmap(mine, 3 * page) == 0);
    CHECK(munmap(shared, 2 * page) == 0);
    CHECK(fsync(fd) == 0);
    char line[8];
    CHECK(lseek(fd, page, SEEK_SET) == static_cast<off_t>(page) && read(fd, line, sizeof(line)) == static_cast<ssize_t>(sizeof(line)));
    CHECK(memcmp(line, "bYWbbZbb", sizeof(line)) == 0);
    close(fd);

    // Shared and writable needs a file open for writing.
    int read_only = open(path, O_RDONLY);
    errno = 0;
    CHECK(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, read_only, 0) == MAP_FAILED && errno == EACCES);
    auto* view = static_cast<char*>(mmap(nullptr, page, PROT_READ, MAP_SHARED, read_only, 0));
    CHECK(view != MAP_FAILED && view[0] == 'a');
    CHECK(munmap(view, page) == 0);
    close(read_only);
    CHECK(unlink(path) == 0);
}

}

int main()
{
    test_mapping("/Users/mapped-test");
    test_mapping("/tmp/mapped-test");

    // Not a file: refused.
    int directory = open("/", O_RDONLY);
    errno = 0;
    CHECK(mmap(nullptr, page, PROT_READ, MAP_PRIVATE, directory, 0) == MAP_FAILED && errno == ENODEV);
    close(directory);
    errno = 0;
    CHECK(mmap(nullptr, page, PROT_READ, MAP_PRIVATE | MAP_SHARED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL);
    return finish();
}
