/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// cat [-nbsuv] [file...]: files (or standard input, "-") to standard output.
static bool s_number = false, s_number_nonblank = false, s_squeeze = false, s_show = false;
static long s_line = 1;
static bool s_at_line_start = true;
static int s_blank_run = 0;

static bool copy_plain(int fd)
{
    char buffer[16384];
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return got == 0;
        for (ssize_t done = 0; done < got;) {
            ssize_t written = write(1, buffer + done, static_cast<size_t>(got - done));
            if (written <= 0)
                return false;
            done += written;
        }
    }
}

static bool copy_formatted(int fd)
{
    FILE* in = fdopen(fd, "r");
    if (!in)
        return false;
    int c;
    while ((c = fgetc(in)) != EOF) {
        if (s_at_line_start) {
            if (c == '\n') {
                if (s_squeeze && ++s_blank_run > 1)
                    continue;
            } else {
                s_blank_run = 0;
            }
            if (s_number && (!s_number_nonblank || c != '\n'))
                printf("%6ld\t", s_line++);
        }
        s_at_line_start = c == '\n';
        if (s_show && c != '\n' && c != '\t' && (c < 0x20 || c == 0x7f)) {
            putchar('^');
            putchar(c == 0x7f ? '?' : c + '@');
            continue;
        }
        putchar(c);
    }
    return true;
}

int main(int argc, char** argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            switch (*option) {
            case 'n':
                s_number = true;
                break;
            case 'b':
                s_number = s_number_nonblank = true;
                break;
            case 's':
                s_squeeze = true;
                break;
            case 'v':
            case 'e':
            case 't':
                s_show = true;
                break;
            case 'u':
                break;
            default:
                fprintf(stderr, "usage: cat [-bnsuv] [file ...]\n");
                return 1;
            }
        }
    }
    bool formatted = s_number || s_squeeze || s_show;
    int status = 0;
    if (first == argc) {
        if (!(formatted ? copy_formatted(0) : copy_plain(0)))
            status = 1;
        return status;
    }
    for (int i = first; i < argc; ++i) {
        int fd = strcmp(argv[i], "-") == 0 ? 0 : open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (!(formatted ? copy_formatted(fd) : copy_plain(fd))) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        if (fd != 0 && !formatted)
            close(fd);
    }
    fflush(stdout);
    return status;
}
