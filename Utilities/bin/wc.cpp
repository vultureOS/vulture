/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <ctype.h>

// wc [-clmw] [file...]: lines, words, bytes, as macOS prints them.
namespace {

bool s_lines = false, s_words = false, s_bytes = false, s_characters = false;
long long s_total_lines = 0, s_total_words = 0, s_total_bytes = 0, s_total_characters = 0;

void print(long long lines, long long words, long long bytes, long long characters, char const* name)
{
    if (s_lines)
        printf(" %7lld", lines);
    if (s_words)
        printf(" %7lld", words);
    if (s_bytes)
        printf(" %7lld", bytes);
    if (s_characters)
        printf(" %7lld", characters);
    if (name)
        printf(" %s", name);
    putchar('\n');
}

void count(FILE* file, char const* name)
{
    long long lines = 0, words = 0, bytes = 0, characters = 0;
    bool in_word = false;
    int c;
    while ((c = fgetc(file)) != EOF) {
        ++bytes;
        if ((c & 0xc0) != 0x80)
            ++characters;
        if (c == '\n')
            ++lines;
        if (isspace(c)) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            ++words;
        }
    }
    s_total_lines += lines;
    s_total_words += words;
    s_total_bytes += bytes;
    s_total_characters += characters;
    print(lines, words, bytes, characters, name);
}

}

int main(int argc, char** argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            switch (*option) {
            case 'l':
                s_lines = true;
                break;
            case 'w':
                s_words = true;
                break;
            case 'c':
                s_bytes = true;
                break;
            case 'm':
                s_characters = true;
                break;
            default:
                fprintf(stderr, "usage: wc [-clmw] [file ...]\n");
                return 1;
            }
        }
    }
    if (!s_lines && !s_words && !s_bytes && !s_characters)
        s_lines = s_words = s_bytes = true;
    int status = 0;
    if (first == argc) {
        count(stdin, nullptr);
        return 0;
    }
    for (int i = first; i < argc; ++i) {
        FILE* file = fopen(argv[i], "r");
        if (!file) {
            warn("wc", argv[i]);
            status = 1;
            continue;
        }
        count(file, argv[i]);
        fclose(file);
    }
    if (argc - first > 1)
        print(s_total_lines, s_total_words, s_total_bytes, s_total_characters, "total");
    return status;
}
