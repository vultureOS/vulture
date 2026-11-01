/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"

// hexdump [-C] [-n length] [-s skip] [file...]; also xxd's simple form.
int main(int argc, char** argv)
{
    bool canonical = false;
    long long limit = -1, skip = 0;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        if (strcmp(argv[first], "-C") == 0)
            canonical = true;
        else if (strcmp(argv[first], "-n") == 0 && first + 1 < argc)
            limit = strtoll(argv[++first], nullptr, 0);
        else if (strcmp(argv[first], "-s") == 0 && first + 1 < argc)
            skip = strtoll(argv[++first], nullptr, 0);
    }
    if (strcmp(last_component(argv[0]), "xxd") == 0)
        canonical = true;
    FILE* file = first < argc ? fopen(argv[first], "r") : stdin;
    if (!file) {
        warn("hexdump", argv[first]);
        return 1;
    }
    for (long long i = 0; i < skip && fgetc(file) != EOF; ++i) {
    }
    unsigned char line[16];
    long long offset = skip;
    for (;;) {
        size_t length = 0;
        int c;
        while (length < 16 && (limit < 0 || offset + static_cast<long long>(length) < skip + limit) && (c = fgetc(file)) != EOF)
            line[length++] = static_cast<unsigned char>(c);
        if (length == 0)
            break;
        if (canonical) {
            printf("%08llx  ", offset);
            for (size_t i = 0; i < 16; ++i) {
                if (i < length)
                    printf("%02x ", line[i]);
                else
                    printf("   ");
                if (i == 7)
                    putchar(' ');
            }
            printf(" |");
            for (size_t i = 0; i < length; ++i)
                putchar(line[i] >= 0x20 && line[i] < 0x7f ? line[i] : '.');
            printf("|\n");
        } else {
            printf("%07llx", offset);
            for (size_t i = 0; i < length; i += 2)
                printf(" %02x%02x", i + 1 < length ? line[i + 1] : 0, line[i]);
            putchar('\n');
        }
        offset += static_cast<long long>(length);
    }
    printf(canonical ? "%08llx\n" : "%07llx\n", offset);
    return 0;
}
