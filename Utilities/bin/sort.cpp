/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <ctype.h>

// sort [-fnru] [-k field[,field]] [-t separator] [-o output] [file...]
namespace {

bool s_numeric = false, s_reverse = false, s_unique = false, s_fold = false;
int s_key_start = 0, s_key_end = 0; // 1-based fields; 0: whole line.
char s_separator = '\0';

// The part of `line` the key covers.
void key_of(char const* line, char const*& start, char const*& end)
{
    start = line;
    end = line + strlen(line);
    if (s_key_start == 0)
        return;
    auto field_start = [&](int field) {
        char const* cursor = line;
        for (int i = 1; i < field && *cursor; ++i) {
            if (s_separator) {
                cursor = strchr(cursor, s_separator);
                if (!cursor)
                    return line + strlen(line);
                ++cursor;
            } else {
                while (*cursor && isspace(static_cast<unsigned char>(*cursor)))
                    ++cursor;
                while (*cursor && !isspace(static_cast<unsigned char>(*cursor)))
                    ++cursor;
            }
        }
        return cursor;
    };
    start = field_start(s_key_start);
    if (s_key_end > 0) {
        char const* after = field_start(s_key_end + 1);
        end = after > start && *after && s_separator ? after - 1 : after;
        if (!s_separator) {
            // Up to the end of field s_key_end.
            char const* cursor = field_start(s_key_end);
            while (*cursor && isspace(static_cast<unsigned char>(*cursor)))
                ++cursor;
            while (*cursor && !isspace(static_cast<unsigned char>(*cursor)))
                ++cursor;
            end = cursor;
        }
    }
}

int compare_lines(char const* a, char const* b)
{
    char const *a_start, *a_end, *b_start, *b_end;
    key_of(a, a_start, a_end);
    key_of(b, b_start, b_end);
    int order;
    if (s_numeric) {
        double x = strtod(a_start, nullptr), y = strtod(b_start, nullptr);
        order = x < y ? -1 : x > y ? 1 : 0;
    } else {
        order = 0;
        for (; a_start < a_end && b_start < b_end; ++a_start, ++b_start) {
            int x = static_cast<unsigned char>(*a_start), y = static_cast<unsigned char>(*b_start);
            if (s_fold) {
                x = toupper(x);
                y = toupper(y);
            }
            if (x != y) {
                order = x - y;
                break;
            }
        }
        if (order == 0)
            order = (a_start < a_end) - (b_start < b_end);
    }
    if (order == 0 && !s_unique && s_key_start != 0)
        order = strcmp(a, b);
    return s_reverse ? -order : order;
}

}

int main(int argc, char** argv)
{
    char const* output = nullptr;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        char const* option = argv[first];
        if (option[1] == 'k' || option[1] == 't' || option[1] == 'o') {
            char const* value = option[2] ? option + 2 : (first + 1 < argc ? argv[++first] : "");
            if (option[1] == 'k') {
                s_key_start = atoi(value);
                char const* comma = strchr(value, ',');
                s_key_end = comma ? atoi(comma + 1) : 0;
            } else if (option[1] == 't') {
                s_separator = value[0];
            } else {
                output = value;
            }
            continue;
        }
        for (char const* flag = option + 1; *flag; ++flag) {
            switch (*flag) {
            case 'n':
                s_numeric = true;
                break;
            case 'r':
                s_reverse = true;
                break;
            case 'u':
                s_unique = true;
                break;
            case 'f':
                s_fold = true;
                break;
            case 'b':
                break;
            default:
                fprintf(stderr, "usage: sort [-bfnru] [-k field] [-t char] [-o output] [file ...]\n");
                return 2;
            }
        }
    }
    size_t count = 0, capacity = 1024;
    auto** lines = static_cast<char**>(malloc(capacity * sizeof(char*)));
    auto read_all = [&](FILE* file) {
        char* line = nullptr;
        size_t size = 0;
        ssize_t length;
        while ((length = getline(&line, &size, file)) >= 0) {
            if (length > 0 && line[length - 1] == '\n')
                line[length - 1] = '\0';
            if (count == capacity) {
                capacity *= 2;
                lines = static_cast<char**>(realloc(lines, capacity * sizeof(char*)));
            }
            lines[count++] = strdup(line);
        }
        free(line);
    };
    if (first == argc)
        read_all(stdin);
    for (int i = first; i < argc; ++i) {
        FILE* file = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");
        if (!file) {
            warn("sort", argv[i]);
            return 2;
        }
        read_all(file);
        if (file != stdin)
            fclose(file);
    }
    qsort(lines, count, sizeof(char*), [](void const* a, void const* b) {
        return compare_lines(*static_cast<char* const*>(a), *static_cast<char* const*>(b));
    });
    FILE* out = output ? fopen(output, "w") : stdout;
    if (!out) {
        warn("sort", output);
        return 2;
    }
    for (size_t i = 0; i < count; ++i) {
        if (s_unique && i > 0 && compare_lines(lines[i - 1], lines[i]) == 0)
            continue;
        fprintf(out, "%s\n", lines[i]);
    }
    if (out != stdout)
        fclose(out);
    fflush(stdout);
    return 0;
}
