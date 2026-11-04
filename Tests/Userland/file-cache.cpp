/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <unistd.h>

// What's read from a disk stays in memory until the memory's wanted for something else: a program that needs it gets it, and the file's
// no different for having had to be read again. Needs a real disk, and not much memory (Meta/compressor-test.sh).

namespace {

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            fprintf(stderr, "file-cache: %d: %s: %s\n", __LINE__, #condition, strerror(errno)); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

constexpr size_t page_size = 4096;
constexpr size_t file_pages = 24 * 1024; // 96 MiB
char const* const path = "/var/tmp/file-cache-test";

unsigned long long statistic(char const* name)
{
    unsigned long long value = 0;
    size_t size = sizeof(value);
    CHECK(sysctlbyname(name, &value, &size, nullptr, 0) == 0);
    return value;
}

void fill(unsigned long long* words, size_t page)
{
    for (size_t i = 0; i < page_size / sizeof(*words); ++i)
        words[i] = page * 0x9e3779b97f4a7c15ull + i;
}

// That won't compress, so it can't be got rid of that way.
void scramble(unsigned char* memory, size_t size, unsigned long long& state)
{
    for (size_t i = 0; i < size; i += 8) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        memcpy(memory + i, &state, 8);
    }
}

}

int main()
{
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    auto const before = statistic("vm.file_cache_pages");
    static unsigned long long page[page_size / sizeof(unsigned long long)];
    for (size_t i = 0; i < file_pages; ++i) {
        fill(page, i);
        CHECK(write(fd, page, page_size) == static_cast<ssize_t>(page_size));
    }
    CHECK(fsync(fd) == 0);
    auto const full = statistic("vm.file_cache_pages");
    printf("file-cache: %llu pages cached before, %llu with the file\n", before, full);
    CHECK(full >= before + file_pages / 2);

    // Until the file's gone from memory, or there's no more to have.
    auto const memory = statistic("hw.memsize");
    size_t const chunk = 16 * 1024 * 1024;
    unsigned long long state = 0x123456789abcdefull;
    unsigned long long lowest = full;
    size_t taken = 0;
    while (taken + chunk < memory * 8 / 10) {
        auto* region = static_cast<unsigned char*>(mmap(nullptr, chunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        CHECK(region != MAP_FAILED);
        scramble(region, chunk, state);
        taken += chunk;
        auto const now = statistic("vm.file_cache_pages");
        if (now < lowest)
            lowest = now;
        if (lowest < full / 2)
            break;
    }
    printf("file-cache: %llu pages cached with %zu MiB taken by a program\n", lowest, taken / (1024 * 1024));
    CHECK(lowest < full / 2);

    // And it's all still there.
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    static unsigned long long expected[page_size / sizeof(unsigned long long)];
    for (size_t i = 0; i < file_pages; ++i) {
        fill(expected, i);
        CHECK(read(fd, page, page_size) == static_cast<ssize_t>(page_size));
        if (memcmp(page, expected, page_size) != 0) {
            fprintf(stderr, "file-cache: page %zu of the file isn't what was written\n", i);
            return 1;
        }
    }
    close(fd);
    CHECK(unlink(path) == 0);
    printf("file-cache: PASS\n");
    return 0;
}
