/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibSystem/Syscall.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// A directory stream: getdents() records read in batches.
struct DIR {
    int fd;
    size_t position;
    size_t length;
    struct dirent current;
    alignas(8) char buffer[4096];
};

extern "C" {

DIR* fdopendir(int fd)
{
    auto* directory = static_cast<DIR*>(calloc(1, sizeof(DIR)));
    if (!directory)
        return nullptr;
    directory->fd = fd;
    return directory;
}

DIR* opendir(char const* path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;
    DIR* directory = fdopendir(fd);
    if (!directory)
        close(fd);
    return directory;
}

struct dirent* readdir(DIR* directory)
{
    if (directory->position >= directory->length) {
        long result = System::syscall(System::Syscall::getdents, static_cast<unsigned long>(directory->fd), reinterpret_cast<unsigned long>(directory->buffer), sizeof(directory->buffer));
        if (result < 0) {
            errno = static_cast<int>(-result);
            return nullptr;
        }
        if (result == 0)
            return nullptr;
        directory->position = 0;
        directory->length = static_cast<size_t>(result);
    }
    auto const* record = reinterpret_cast<vulture_dirent const*>(directory->buffer + directory->position);
    directory->position += record->d_reclen;
    directory->current.d_ino = record->d_ino;
    directory->current.d_type = record->d_type;
    strlcpy(directory->current.d_name, record->d_name, sizeof(directory->current.d_name));
    return &directory->current;
}

void rewinddir(DIR* directory)
{
    lseek(directory->fd, 0, SEEK_SET);
    directory->position = directory->length = 0;
}

int closedir(DIR* directory)
{
    int result = close(directory->fd);
    free(directory);
    return result;
}

int dirfd(DIR* directory)
{
    return directory->fd;
}
}
