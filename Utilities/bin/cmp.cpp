/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"

// cmp [-l | -s] file1 file2 [skip1 [skip2]]: exit status 0 if they're the same, 1 if they differ, 2 for trouble.
int main(int argc, char** argv)
{
    bool list = false, silent = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'l')
                list = true;
            else if (*option == 's')
                silent = true;
            else {
                fprintf(stderr, "usage: cmp [-l | -s] file1 file2 [skip1 [skip2]]\n");
                return 2;
            }
        }
    }
    if (argc - first < 2 || argc - first > 4) {
        fprintf(stderr, "usage: cmp [-l | -s] file1 file2 [skip1 [skip2]]\n");
        return 2;
    }
    char const* names[2] = { argv[first], argv[first + 1] };
    FILE* files[2];
    for (int i = 0; i < 2; ++i) {
        files[i] = strcmp(names[i], "-") == 0 ? stdin : fopen(names[i], "r");
        if (!files[i]) {
            if (!silent)
                warn("cmp", names[i]);
            return 2;
        }
        if (argc - first > 2 + i) {
            for (long skip = strtol(argv[first + 2 + i], nullptr, 0); skip > 0; --skip) {
                if (fgetc(files[i]) == EOF)
                    break;
            }
        }
    }
    long byte = 1, line = 1;
    bool differ = false;
    for (;; ++byte) {
        int const a = fgetc(files[0]);
        int const b = fgetc(files[1]);
        if (a == EOF || b == EOF) {
            if (a == EOF && b == EOF)
                return differ ? 1 : 0;
            if (!silent)
                fprintf(stderr, "cmp: EOF on %s\n", a == EOF ? names[0] : names[1]);
            return 1;
        }
        if (a != b) {
            differ = true;
            if (silent)
                return 1;
            if (!list) {
                printf("%s %s differ: char %ld, line %ld\n", names[0], names[1], byte, line);
                return 1;
            }
            printf("%6ld %3o %3o\n", byte, a, b);
        }
        if (a == '\n')
            ++line;
    }
}
