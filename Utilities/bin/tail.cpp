/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// tail [-f] [-n [+]lines | -c [+]bytes] [file...]
namespace {

bool s_follow = false;

void tail_stream(FILE* file, long count, bool bytes, bool from_start)
{
    if (from_start) {
        int c;
        long seen = 1;
        while ((c = fgetc(file)) != EOF) {
            if (seen >= count)
                putchar(c);
            if (bytes || c == '\n')
                ++seen;
        }
        return;
    }
    // Keep the whole input (simple; files are small), then print its end.
    size_t capacity = 65536, length = 0;
    auto* data = static_cast<char*>(malloc(capacity));
    int c;
    while ((c = fgetc(file)) != EOF) {
        if (length == capacity) {
            capacity *= 2;
            data = static_cast<char*>(realloc(data, capacity));
        }
        data[length++] = static_cast<char>(c);
    }
    size_t start = length;
    if (bytes) {
        start = length > static_cast<size_t>(count) ? length - static_cast<size_t>(count) : 0;
    } else {
        long lines = 0;
        size_t position = length;
        if (position > 0 && data[position - 1] == '\n')
            --position;
        while (position > 0) {
            if (data[position - 1] == '\n' && ++lines == count)
                break;
            --position;
        }
        start = position;
    }
    fwrite(data + start, 1, length - start, stdout);
    free(data);
}

}

int main(int argc, char** argv)
{
    long count = 10;
    bool bytes = false, from_start = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        char const* option = argv[first];
        if (option[1] == 'f') {
            s_follow = true;
            continue;
        }
        if (option[1] == 'n' || option[1] == 'c') {
            bytes = option[1] == 'c';
            char const* value = option[2] ? option + 2 : (first + 1 < argc ? argv[++first] : "10");
            from_start = value[0] == '+';
            count = strtol(value + (from_start || value[0] == '-' ? 1 : 0), nullptr, 10);
            continue;
        }
        if (option[1] >= '0' && option[1] <= '9') {
            count = strtol(option + 1, nullptr, 10);
            continue;
        }
        fprintf(stderr, "usage: tail [-f] [-n lines | -c bytes] [file ...]\n");
        return 1;
    }
    int files = argc - first;
    int status = 0;
    if (files == 0) {
        tail_stream(stdin, count, bytes, from_start);
        fflush(stdout);
        return 0;
    }
    for (int i = first; i < argc; ++i) {
        FILE* file = fopen(argv[i], "r");
        if (!file) {
            warn("tail", argv[i]);
            status = 1;
            continue;
        }
        if (files > 1)
            printf("%s==> %s <==\n", i > first ? "\n" : "", argv[i]);
        tail_stream(file, count, bytes, from_start);
        fflush(stdout);
        if (s_follow && i == argc - 1) {
            // Print what is appended, until interrupted.
            int fd = fileno(file);
            off_t position = lseek(fd, 0, SEEK_END);
            for (;;) {
                struct stat information;
                if (fstat(fd, &information) == 0 && information.st_size > position) {
                    char buffer[4096];
                    lseek(fd, position, SEEK_SET);
                    ssize_t got = read(fd, buffer, sizeof(buffer));
                    if (got > 0) {
                        (void)write(STDOUT_FILENO, buffer, static_cast<size_t>(got));
                        position += got;
                        continue;
                    }
                } else if (fstat(fd, &information) == 0 && information.st_size < position) {
                    position = 0; // Truncated.
                }
                struct timespec pause { 0, 250 * 1000 * 1000 };
                nanosleep(&pause, nullptr);
            }
        }
        fclose(file);
    }
    return status;
}
