/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>
#include <regex.h>
#include <sys/stat.h>
#include <unistd.h>

// grep [-EFciHhilnoqrsvwx] [-e pattern]... [pattern] [file...]
namespace {

bool s_extended = false, s_fixed = false, s_count = false, s_ignore_case = false, s_list_files = false, s_line_numbers = false;
bool s_only_matching = false, s_quiet = false, s_recursive = false, s_suppress_errors = false, s_invert = false;
bool s_words = false, s_whole_lines = false, s_color = false;
int s_names = -1; // -1: decide by file count; 0: never (-h); 1: always (-H).
char* s_patterns[64];
int s_pattern_count = 0;
regex_t s_compiled[64];
bool s_matched_any = false;

bool is_word_character(char c)
{
    return isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The first match in `line` at or after `from`: [start, end).
bool find(char const* line, size_t from, size_t& start, size_t& end)
{
    bool found = false;
    for (int p = 0; p < s_pattern_count; ++p) {
        size_t candidate_start = 0, candidate_end = 0;
        bool hit = false;
        if (s_fixed) {
            char const* pattern = s_patterns[p];
            size_t length = strlen(pattern);
            for (char const* cursor = line + from; *cursor; ++cursor) {
                if ((s_ignore_case ? strncasecmp(cursor, pattern, length) : strncmp(cursor, pattern, length)) == 0) {
                    candidate_start = static_cast<size_t>(cursor - line);
                    candidate_end = candidate_start + length;
                    hit = true;
                    break;
                }
            }
            if (length == 0) {
                candidate_start = candidate_end = from;
                hit = true;
            }
        } else {
            regmatch_t match;
            if (regexec(&s_compiled[p], line + from, 1, &match, from > 0 ? REG_NOTBOL : 0) == 0) {
                candidate_start = from + static_cast<size_t>(match.rm_so);
                candidate_end = from + static_cast<size_t>(match.rm_eo);
                hit = true;
            }
        }
        if (!hit)
            continue;
        if (s_words) {
            bool before = candidate_start == 0 || !is_word_character(line[candidate_start - 1]);
            bool after = !is_word_character(line[candidate_end]);
            if (!before || !after) {
                // Try further along the line.
                size_t next_start, next_end;
                if (line[candidate_start] && find(line, candidate_start + 1, next_start, next_end)) {
                    candidate_start = next_start;
                    candidate_end = next_end;
                } else {
                    continue;
                }
            }
        }
        if (s_whole_lines && (candidate_start != 0 || line[candidate_end] != '\0'))
            continue;
        if (!found || candidate_start < start) {
            start = candidate_start;
            end = candidate_end;
            found = true;
        }
    }
    return found;
}

void grep_stream(FILE* file, char const* name, bool show_name)
{
    char* line = nullptr;
    size_t capacity = 0;
    ssize_t length;
    long line_number = 0, count = 0;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        ++line_number;
        if (length > 0 && line[length - 1] == '\n')
            line[--length] = '\0';
        size_t start = 0, end = 0;
        bool matched = find(line, 0, start, end) != s_invert;
        if (!matched)
            continue;
        ++count;
        s_matched_any = true;
        if (s_quiet) {
            free(line);
            return;
        }
        if (s_list_files) {
            printf("%s\n", name);
            break;
        }
        if (s_count)
            continue;
        if (s_only_matching && !s_invert) {
            size_t from = 0;
            while (from <= static_cast<size_t>(length) && find(line, from, start, end)) {
                if (end == start) {
                    from = end + 1;
                    continue;
                }
                if (show_name)
                    printf("%s:", name);
                if (s_line_numbers)
                    printf("%ld:", line_number);
                printf("%.*s\n", static_cast<int>(end - start), line + start);
                from = end;
            }
            continue;
        }
        if (show_name)
            printf(s_color ? "\x1b[35m%s\x1b[36m:\x1b[0m" : "%s:", name);
        if (s_line_numbers)
            printf(s_color ? "\x1b[32m%ld\x1b[36m:\x1b[0m" : "%ld:", line_number);
        if (s_color && !s_invert) {
            size_t from = 0;
            while (find(line, from, start, end) && end > start) {
                printf("%.*s\x1b[1;31m%.*s\x1b[0m", static_cast<int>(start - from), line + from, static_cast<int>(end - start), line + start);
                from = end;
            }
            printf("%s\n", line + from);
        } else {
            printf("%s\n", line);
        }
    }
    if (s_count)
        show_name ? printf("%s:%ld\n", name, count) : printf("%ld\n", count);
    free(line);
}

void grep_path(char const* path, bool show_name)
{
    if (strcmp(path, "-") == 0) {
        grep_stream(stdin, "(standard input)", show_name);
        return;
    }
    struct stat information;
    if (stat(path, &information) < 0) {
        if (!s_suppress_errors)
            warn("grep", path);
        return;
    }
    if (S_ISDIR(information.st_mode)) {
        if (!s_recursive) {
            if (!s_suppress_errors)
                fprintf(stderr, "grep: %s: Is a directory\n", path);
            return;
        }
        DIR* directory = opendir(path);
        if (!directory)
            return;
        while (struct dirent* entry = readdir(directory)) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            char* child = path_join(path, entry->d_name);
            grep_path(child, show_name);
            free(child);
        }
        closedir(directory);
        return;
    }
    FILE* file = fopen(path, "r");
    if (!file) {
        if (!s_suppress_errors)
            warn("grep", path);
        return;
    }
    grep_stream(file, path, show_name);
    fclose(file);
}

}

int main(int argc, char** argv)
{
    if (strcmp(last_component(argv[0]), "egrep") == 0)
        s_extended = true;
    if (strcmp(last_component(argv[0]), "fgrep") == 0)
        s_fixed = true;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "--") == 0) {
            ++first;
            break;
        }
        if (strcmp(argv[first], "--color") == 0 || strcmp(argv[first], "--colour") == 0 || strcmp(argv[first], "--color=auto") == 0) {
            s_color = isatty(STDOUT_FILENO);
            continue;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            switch (*option) {
            case 'E':
                s_extended = true;
                break;
            case 'F':
                s_fixed = true;
                break;
            case 'G':
                s_extended = false;
                break;
            case 'c':
                s_count = true;
                break;
            case 'i':
            case 'y':
                s_ignore_case = true;
                break;
            case 'l':
                s_list_files = true;
                break;
            case 'n':
                s_line_numbers = true;
                break;
            case 'o':
                s_only_matching = true;
                break;
            case 'q':
                s_quiet = true;
                break;
            case 'r':
            case 'R':
                s_recursive = true;
                break;
            case 's':
                s_suppress_errors = true;
                break;
            case 'v':
                s_invert = true;
                break;
            case 'w':
                s_words = true;
                break;
            case 'x':
                s_whole_lines = true;
                break;
            case 'H':
                s_names = 1;
                break;
            case 'h':
                s_names = 0;
                break;
            case 'e':
                if (option[1]) {
                    s_patterns[s_pattern_count++] = const_cast<char*>(option + 1);
                } else if (first + 1 < argc) {
                    s_patterns[s_pattern_count++] = argv[++first];
                }
                option = " ";
                goto next_argument;
            default:
                fprintf(stderr, "usage: grep [-EFHchilnoqrsvwx] [-e pattern] [pattern] [file ...]\n");
                return 2;
            }
        }
    next_argument:;
    }
    if (s_pattern_count == 0) {
        if (first >= argc) {
            fprintf(stderr, "usage: grep [-EFHchilnoqrsvwx] [-e pattern] [pattern] [file ...]\n");
            return 2;
        }
        s_patterns[s_pattern_count++] = argv[first++];
    }
    if (!s_fixed) {
        for (int p = 0; p < s_pattern_count; ++p) {
            int error = regcomp(&s_compiled[p], s_patterns[p], (s_extended ? REG_EXTENDED : 0) | (s_ignore_case ? REG_ICASE : 0));
            if (error) {
                char message[128];
                regerror(error, &s_compiled[p], message, sizeof(message));
                fprintf(stderr, "grep: %s: %s\n", s_patterns[p], message);
                return 2;
            }
        }
    }
    int file_count = argc - first;
    bool show_name = s_names == 1 || (s_names == -1 && (file_count > 1 || s_recursive));
    if (file_count == 0)
        grep_path(s_recursive ? "." : "-", show_name);
    for (int i = first; i < argc; ++i)
        grep_path(argv[i], show_name);
    fflush(stdout);
    return s_matched_any ? 0 : 1;
}
