/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

// tee [-ai] [file...]: standard input to standard output and the files.
int main(int argc, char** argv)
{
    bool append = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'a')
                append = true;
            else if (*option == 'i')
                signal(SIGINT, SIG_IGN);
        }
    }
    int fds[64];
    int count = 0, status = 0;
    fds[count++] = STDOUT_FILENO;
    for (int i = first; i < argc && count < 64; ++i) {
        int fd = open(argv[i], O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0666);
        if (fd < 0) {
            warn("tee", argv[i]);
            status = 1;
            continue;
        }
        fds[count++] = fd;
    }
    char buffer[8192];
    ssize_t got;
    while ((got = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < count; ++i) {
            if (fds[i] >= 0 && write(fds[i], buffer, static_cast<size_t>(got)) != got)
                status = 1;
        }
    }
    return status;
}
