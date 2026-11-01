/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <unistd.h>

// yes [string]: the string ("y") over and over.
int main(int argc, char** argv)
{
    char line[1024];
    size_t length = 0;
    for (int i = 1; i < argc; ++i) {
        size_t piece = strlen(argv[i]);
        if (length + piece + 2 >= sizeof(line))
            break;
        if (i > 1)
            line[length++] = ' ';
        memcpy(line + length, argv[i], piece);
        length += piece;
    }
    if (argc < 2)
        line[length++] = 'y';
    line[length++] = '\n';
    char buffer[8192];
    size_t filled = 0;
    while (filled + length <= sizeof(buffer)) {
        memcpy(buffer + filled, line, length);
        filled += length;
    }
    for (;;) {
        if (write(STDOUT_FILENO, buffer, filled) < 0)
            return 1;
    }
}
