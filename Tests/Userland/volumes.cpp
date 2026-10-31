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

// The EFI System Partition, read through the FAT driver at /Volumes/EFI.
int main()
{
    struct stat information {};
    CHECK(stat("/Volumes/EFI", &information) == 0 && S_ISDIR(information.st_mode));

    bool saw_efi = false, saw_system = false;
    DIR* directory = opendir("/Volumes/EFI");
    CHECK(directory != nullptr);
    while (auto* entry = readdir(directory)) {
        saw_efi = saw_efi || strcmp(entry->d_name, "EFI") == 0;
        saw_system = saw_system || strcmp(entry->d_name, "System") == 0;
    }
    closedir(directory);
    CHECK(saw_efi && saw_system);

    // The loader is a PE image; FAT names match without regard to case.
    char header[2] {};
    int loader = open("/Volumes/EFI/efi/boot/bootaa64.efi", O_RDONLY);
    CHECK(loader >= 0);
    CHECK(read(loader, header, 2) == 2 && header[0] == 'M' && header[1] == 'Z');
    close(loader);

    // The kernel, read whole: it spans many clusters.
    int kernel = open("/Volumes/EFI/System/Library/Kernels/vulture", O_RDONLY);
    CHECK(kernel >= 0);
    CHECK(fstat(kernel, &information) == 0 && S_ISREG(information.st_mode) && information.st_size > 65536);
    auto size = static_cast<size_t>(information.st_size);
    auto* contents = static_cast<unsigned char*>(malloc(size));
    CHECK(contents != nullptr);
    size_t total = 0;
    for (;;) {
        ssize_t got = read(kernel, contents + total, size - total < 10000 ? size - total : 10000);
        CHECK(got >= 0);
        if (got <= 0)
            break;
        total += static_cast<size_t>(got);
    }
    CHECK(total == size);
    CHECK(contents[0] == 0x7f && contents[1] == 'E' && contents[2] == 'L' && contents[3] == 'F');
    // Reading from the middle gives the same bytes.
    unsigned char middle[300];
    CHECK(lseek(kernel, static_cast<off_t>(size / 2), SEEK_SET) == static_cast<off_t>(size / 2));
    CHECK(read(kernel, middle, sizeof(middle)) == static_cast<ssize_t>(sizeof(middle)));
    CHECK(memcmp(middle, contents + size / 2, sizeof(middle)) == 0);
    free(contents);
    close(kernel);

    // ".." leaves the volume back into the root filesystem.
    struct stat volumes {}, parent {};
    CHECK(stat("/Volumes", &volumes) == 0 && stat("/Volumes/EFI/..", &parent) == 0);
    CHECK(volumes.st_ino == parent.st_ino && volumes.st_dev == parent.st_dev);

    // The volume is read-only.
    errno = 0;
    CHECK(open("/Volumes/EFI/new-file", O_WRONLY | O_CREAT, 0644) < 0 && errno == EROFS);
    CHECK(mkdir("/Volumes/EFI/new-directory", 0755) < 0 && errno == EROFS);
    CHECK(open("/Volumes/EFI/missing", O_RDONLY) < 0 && errno == ENOENT);
    return finish();
}
