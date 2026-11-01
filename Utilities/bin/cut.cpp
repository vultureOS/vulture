/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"

// cut -b list | -c list | -f list [-d delimiter] [-s] [file...]
namespace {

bool s_selected[4096];
bool s_open_ended = false;
size_t s_open_from = 0;

bool selected(size_t position)
{
    if (s_open_ended && position >= s_open_from)
        return true;
    return position < sizeof(s_selected) && s_selected[position];
}

bool parse_list(char const* list)
{
    for (char const* cursor = list; *cursor;) {
        size_t low = 1, high;
        if (*cursor != '-')
            low = strtoul(cursor, const_cast<char**>(&cursor), 10);
        if (*cursor == '-') {
            ++cursor;
            if (*cursor >= '0' && *cursor <= '9') {
                high = strtoul(cursor, const_cast<char**>(&cursor), 10);
            } else {
                s_open_ended = true;
                s_open_from = low;
                high = low - 1;
            }
        } else {
            high = low;
        }
        if (low == 0)
            return false;
        for (size_t i = low; i <= high && i < sizeof(s_selected); ++i)
            s_selected[i] = true;
        if (*cursor == ',')
            ++cursor;
        else if (*cursor)
            return false;
    }
    return true;
}

}

int main(int argc, char** argv)
{
    char mode = 0, delimiter = '\t';
    bool only_delimited = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        char option = argv[first][1];
        if (option == 's') {
            only_delimited = true;
            continue;
        }
        char const* value = argv[first][2] ? argv[first] + 2 : (first + 1 < argc ? argv[++first] : "");
        if (option == 'b' || option == 'c' || option == 'f') {
            mode = option;
            if (!parse_list(value)) {
                fprintf(stderr, "cut: [-bcf] list: illegal list value\n");
                return 1;
            }
        } else if (option == 'd') {
            delimiter = value[0];
        } else {
            fprintf(stderr, "usage: cut -b list [file ...]\n       cut -c list [file ...]\n       cut -f list [-s] [-d delim] [file ...]\n");
            return 1;
        }
    }
    if (!mode) {
        fprintf(stderr, "usage: cut -b list [file ...]\n       cut -c list [file ...]\n       cut -f list [-s] [-d delim] [file ...]\n");
        return 1;
    }
    int status = 0;
    for (int i = first; i < argc || i == first; ++i) {
        FILE* file = i >= argc || strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");
        if (!file) {
            warn("cut", argv[i]);
            status = 1;
            continue;
        }
        char* line = nullptr;
        size_t size = 0;
        ssize_t length;
        while ((length = getline(&line, &size, file)) >= 0) {
            if (length > 0 && line[length - 1] == '\n')
                line[--length] = '\0';
            if (mode != 'f') {
                for (ssize_t j = 0; j < length; ++j) {
                    if (selected(static_cast<size_t>(j) + 1))
                        putchar(line[j]);
                }
                putchar('\n');
                continue;
            }
            if (!strchr(line, delimiter)) {
                if (!only_delimited)
                    printf("%s\n", line);
                continue;
            }
            size_t field = 1;
            bool printed = false;
            char* start = line;
            for (char* cursor = line;; ++cursor) {
                if (*cursor == delimiter || *cursor == '\0') {
                    bool end = *cursor == '\0';
                    if (selected(field)) {
                        if (printed)
                            putchar(delimiter);
                        fwrite(start, 1, static_cast<size_t>(cursor - start), stdout);
                        printed = true;
                    }
                    if (end)
                        break;
                    ++field;
                    start = cursor + 1;
                }
            }
            putchar('\n');
        }
        free(line);
        if (file != stdin)
            fclose(file);
        if (i >= argc)
            break;
    }
    fflush(stdout);
    return status;
}
