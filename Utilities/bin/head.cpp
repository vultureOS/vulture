/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"

// head [-n lines | -c bytes] [file...]
int main(int argc, char** argv)
{
    long lines = 10, bytes = -1;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        char const* option = argv[first];
        if ((option[1] == 'n' || option[1] == 'c')) {
            char const* value = option[2] ? option + 2 : (first + 1 < argc ? argv[++first] : nullptr);
            if (!value) {
                fprintf(stderr, "usage: head [-n lines | -c bytes] [file ...]\n");
                return 1;
            }
            (option[1] == 'n' ? lines : bytes) = strtol(value, nullptr, 10);
        } else if (option[1] >= '0' && option[1] <= '9') {
            lines = strtol(option + 1, nullptr, 10);
        } else {
            fprintf(stderr, "usage: head [-n lines | -c bytes] [file ...]\n");
            return 1;
        }
    }
    int status = 0;
    int count = argc - first;
    for (int i = first; i < argc || (count == 0 && i == first); ++i) {
        FILE* file = count == 0 || strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");
        if (!file) {
            warn("head", argv[i]);
            status = 1;
            continue;
        }
        if (count > 1)
            printf("%s==> %s <==\n", i > first ? "\n" : "", argv[i]);
        int c;
        if (bytes >= 0) {
            for (long n = 0; n < bytes && (c = fgetc(file)) != EOF; ++n)
                putchar(c);
        } else {
            for (long n = 0; n < lines && (c = fgetc(file)) != EOF;) {
                putchar(c);
                if (c == '\n')
                    ++n;
            }
        }
        if (file != stdin)
            fclose(file);
        if (count == 0)
            break;
    }
    fflush(stdout);
    return status;
}
