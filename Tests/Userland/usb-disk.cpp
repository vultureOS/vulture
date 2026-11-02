/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// A USB stick: Meta/hardware-test.sh makes one with a known first block, and looks at what this left on
// it afterwards, from outside. Across more than the largest single transfer, to go through all the paths.

namespace {

constexpr char const signature[] = "vultureOS USB test stick";
constexpr size_t block_size = 512;
constexpr size_t test_blocks = 400; // 200 KiB, past 64 KiB at a time.
constexpr size_t test_first_block = 8;

char const* find_stick()
{
    static char path[32];
    for (int disk = 0; disk < 8; ++disk) {
        snprintf(path, sizeof(path), "/dev/disk%d", disk);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        char block[block_size] {};
        bool const is_it = read(fd, block, sizeof(block)) == static_cast<ssize_t>(sizeof(block)) && memcmp(block, signature, sizeof(signature)) == 0;
        close(fd);
        if (is_it)
            return path;
    }
    return nullptr;
}

}

int main()
{
    auto const* path = find_stick();
    CHECK(path != nullptr);
    if (!path)
        return finish();
    printf("usb-disk: found the stick at %s\n", path);
    int fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    static unsigned char blocks[test_blocks * block_size];
    for (size_t i = 0; i < sizeof(blocks); ++i)
        blocks[i] = static_cast<unsigned char>(i * 31 + i / block_size);
    CHECK(lseek(fd, test_first_block * block_size, SEEK_SET) >= 0);
    CHECK(write(fd, blocks, sizeof(blocks)) == static_cast<ssize_t>(sizeof(blocks)));
    fsync(fd);
    static unsigned char back[sizeof(blocks)];
    CHECK(lseek(fd, test_first_block * block_size, SEEK_SET) >= 0);
    CHECK(read(fd, back, sizeof(back)) == static_cast<ssize_t>(sizeof(back)));
    CHECK(memcmp(blocks, back, sizeof(blocks)) == 0);
    close(fd);
    printf("usb-disk: read back what was written\n");
    return finish();
}
