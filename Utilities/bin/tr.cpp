/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <ctype.h>

// tr [-cds] string1 [string2]: ranges a-z, classes [:upper:] etc., escapes.
namespace {

size_t expand(char const* spec, unsigned char* out, size_t capacity)
{
    size_t length = 0;
    auto add = [&](int c) {
        if (length < capacity)
            out[length++] = static_cast<unsigned char>(c);
    };
    for (char const* cursor = spec; *cursor;) {
        if (cursor[0] == '[' && cursor[1] == ':') {
            char const* end = strstr(cursor, ":]");
            if (end) {
                char name[16] = {};
                memcpy(name, cursor + 2, min<size_t>(static_cast<size_t>(end - cursor - 2), 15));
                for (int c = 0; c < 256; ++c) {
                    bool member = (strcmp(name, "upper") == 0 && isupper(c)) || (strcmp(name, "lower") == 0 && islower(c))
                        || (strcmp(name, "alpha") == 0 && isalpha(c)) || (strcmp(name, "digit") == 0 && isdigit(c))
                        || (strcmp(name, "alnum") == 0 && isalnum(c)) || (strcmp(name, "space") == 0 && isspace(c))
                        || (strcmp(name, "punct") == 0 && ispunct(c)) || (strcmp(name, "blank") == 0 && (c == ' ' || c == '\t'));
                    if (member)
                        add(c);
                }
                cursor = end + 2;
                continue;
            }
        }
        int c = static_cast<unsigned char>(*cursor++);
        if (c == '\\' && *cursor) {
            char escape = *cursor++;
            c = escape == 'n' ? '\n' : escape == 't' ? '\t' : escape == 'r' ? '\r' : escape == '\\' ? '\\' : escape;
            if (escape >= '0' && escape <= '7') {
                c = escape - '0';
                for (int digits = 1; digits < 3 && *cursor >= '0' && *cursor <= '7'; ++digits)
                    c = c * 8 + (*cursor++ - '0');
            }
        }
        if (cursor[0] == '-' && cursor[1]) {
            int high = static_cast<unsigned char>(cursor[1]);
            cursor += 2;
            for (int x = c; x <= high; ++x)
                add(x);
            continue;
        }
        add(c);
    }
    return length;
}

}

int main(int argc, char** argv)
{
    bool complement = false, remove = false, squeeze = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'c' || *option == 'C')
                complement = true;
            else if (*option == 'd')
                remove = true;
            else if (*option == 's')
                squeeze = true;
        }
    }
    if (first >= argc) {
        fprintf(stderr, "usage: tr [-cs] string1 string2\n       tr [-c] -d string1\n       tr [-c] -s string1\n       tr [-c] -ds string1 string2\n");
        return 1;
    }
    unsigned char set1[256], set2[256];
    size_t length1 = expand(argv[first], set1, sizeof(set1));
    size_t length2 = first + 1 < argc ? expand(argv[first + 1], set2, sizeof(set2)) : 0;
    bool in1[256] = {}, in2[256] = {};
    for (size_t i = 0; i < length1; ++i)
        in1[set1[i]] = true;
    if (complement) {
        for (int c = 0; c < 256; ++c)
            in1[c] = !in1[c];
    }
    int map[256];
    for (int c = 0; c < 256; ++c)
        map[c] = c;
    if (!remove && length2 > 0) {
        if (complement) {
            for (int c = 0; c < 256; ++c) {
                if (in1[c])
                    map[c] = set2[length2 - 1];
            }
        } else {
            for (size_t i = 0; i < length1; ++i)
                map[set1[i]] = set2[i < length2 ? i : length2 - 1];
        }
    }
    unsigned char const* squeeze_set = remove ? set2 : (length2 > 0 ? set2 : set1);
    size_t squeeze_length = remove ? length2 : (length2 > 0 ? length2 : length1);
    for (size_t i = 0; i < squeeze_length; ++i)
        in2[squeeze_set[i]] = true;
    if (squeeze && !remove && length2 == 0) {
        for (int c = 0; c < 256; ++c)
            in2[c] = in1[c];
    }
    int c, last = -1;
    while ((c = getchar()) != EOF) {
        if (remove && in1[c])
            continue;
        int out = remove ? c : map[c];
        if (squeeze && in2[out] && out == last)
            continue;
        putchar(out);
        last = out;
    }
    fflush(stdout);
    return 0;
}
