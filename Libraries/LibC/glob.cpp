/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Matches a bracket expression at `pattern` (just past '['). Returns where
// it ends (past ']'), or null if the pattern has no closing bracket.
char const* match_bracket(char const* pattern, char character, int flags, bool& matched)
{
    bool negate = *pattern == '!' || *pattern == '^';
    if (negate)
        ++pattern;
    bool found = false;
    bool first = true;
    auto fold = [&](char c) { return (flags & FNM_CASEFOLD) ? static_cast<char>(tolower(static_cast<unsigned char>(c))) : c; };
    while (*pattern && (first || *pattern != ']')) {
        first = false;
        char low = *pattern;
        if (low == '\\' && !(flags & FNM_NOESCAPE) && pattern[1])
            low = *++pattern;
        ++pattern;
        char high = low;
        if (*pattern == '-' && pattern[1] && pattern[1] != ']') {
            high = pattern[1];
            if (high == '\\' && !(flags & FNM_NOESCAPE) && pattern[2]) {
                high = pattern[2];
                ++pattern;
            }
            pattern += 2;
        }
        if (fold(character) >= fold(low) && fold(character) <= fold(high))
            found = true;
    }
    if (*pattern != ']')
        return nullptr;
    matched = found != negate;
    return pattern + 1;
}

bool match(char const* pattern, char const* string, int flags, bool at_start)
{
    auto fold = [&](char c) { return (flags & FNM_CASEFOLD) ? tolower(static_cast<unsigned char>(c)) : static_cast<unsigned char>(c); };
    while (*pattern) {
        // A leading period must be matched by a period (FNM_PERIOD).
        bool leading = (flags & FNM_PERIOD) && *string == '.' && (at_start || ((flags & FNM_PATHNAME) && string[-1] == '/'));
        switch (*pattern) {
        case '*': {
            while (*pattern == '*')
                ++pattern;
            if (leading)
                return false;
            if (!*pattern)
                return !(flags & FNM_PATHNAME) || !strchr(string, '/');
            for (char const* rest = string; *rest; ++rest) {
                if (match(pattern, rest, flags, false))
                    return true;
                if ((flags & FNM_PATHNAME) && *rest == '/')
                    return false;
            }
            return match(pattern, string + strlen(string), flags, false);
        }
        case '?':
            if (!*string || leading || ((flags & FNM_PATHNAME) && *string == '/'))
                return false;
            ++pattern;
            ++string;
            at_start = false;
            break;
        case '[': {
            if (!*string || leading || ((flags & FNM_PATHNAME) && *string == '/'))
                return false;
            bool matched = false;
            char const* end = match_bracket(pattern + 1, *string, flags, matched);
            if (!end) {
                // No closing bracket: a literal '['.
                if (*string != '[')
                    return false;
                ++pattern;
            } else {
                if (!matched)
                    return false;
                pattern = end;
            }
            ++string;
            at_start = false;
            break;
        }
        case '\\':
            if (!(flags & FNM_NOESCAPE) && pattern[1])
                ++pattern;
            [[fallthrough]];
        default:
            if (fold(*pattern) != fold(*string))
                return false;
            ++pattern;
            ++string;
            at_start = false;
        }
    }
    return *string == '\0';
}

bool has_wildcards(char const* text, size_t length, bool escapes)
{
    for (size_t i = 0; i < length; ++i) {
        if (text[i] == '\\' && escapes) {
            ++i;
            continue;
        }
        if (text[i] == '*' || text[i] == '?' || text[i] == '[')
            return true;
    }
    return false;
}

struct Results {
    char** paths { nullptr };
    size_t count { 0 };
    size_t capacity { 0 };

    bool add(char const* path, size_t length)
    {
        if (count + 1 >= capacity) {
            size_t grown = capacity ? capacity * 2 : 16;
            auto** bigger = static_cast<char**>(realloc(paths, grown * sizeof(char*)));
            if (!bigger)
                return false;
            paths = bigger;
            capacity = grown;
        }
        auto* copy = static_cast<char*>(malloc(length + 1));
        if (!copy)
            return false;
        memcpy(copy, path, length);
        copy[length] = '\0';
        paths[count++] = copy;
        return true;
    }
};

// Expands `pattern` from `position` on, `prefix` being the directory
// reached so far.
int expand(char* prefix, size_t prefix_length, char const* pattern, int flags, int (*error_function)(char const*, int), Results& results)
{
    // Skip slashes into the prefix.
    while (*pattern == '/') {
        if (prefix_length + 1 >= PATH_MAX)
            return 0;
        prefix[prefix_length++] = '/';
        ++pattern;
    }
    prefix[prefix_length] = '\0';
    if (!*pattern) {
        struct stat information;
        if (stat(prefix, &information) < 0 && lstat(prefix, &information) < 0)
            return 0;
        size_t length = prefix_length;
        if ((flags & GLOB_MARK) && S_ISDIR(information.st_mode) && length > 0 && prefix[length - 1] != '/' && length + 1 < PATH_MAX) {
            prefix[length++] = '/';
            prefix[length] = '\0';
        }
        return results.add(prefix, length) ? 0 : GLOB_NOSPACE;
    }
    char const* end = strchr(pattern, '/');
    size_t component_length = end ? static_cast<size_t>(end - pattern) : strlen(pattern);
    char const* rest = pattern + component_length;
    bool escapes = !(flags & GLOB_NOESCAPE);

    if (!has_wildcards(pattern, component_length, escapes)) {
        // Literal: just append (without the escapes).
        size_t length = prefix_length;
        for (size_t i = 0; i < component_length && length + 1 < PATH_MAX; ++i) {
            if (pattern[i] == '\\' && escapes && i + 1 < component_length)
                ++i;
            prefix[length++] = pattern[i];
        }
        prefix[length] = '\0';
        struct stat information;
        if (lstat(prefix, &information) < 0)
            return 0;
        return expand(prefix, length, rest, flags, error_function, results);
    }

    char component[NAME_MAX + 1];
    if (component_length > NAME_MAX)
        return 0;
    memcpy(component, pattern, component_length);
    component[component_length] = '\0';
    DIR* directory = opendir(prefix_length ? prefix : ".");
    if (!directory) {
        if ((error_function && error_function(prefix_length ? prefix : ".", errno)) || (flags & GLOB_ERR))
            return GLOB_ABORTED;
        return 0;
    }
    int result = 0;
    while (struct dirent* entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            if (component[0] != '.')
                continue;
        }
        if (fnmatch(component, entry->d_name, FNM_PERIOD | (escapes ? 0 : FNM_NOESCAPE)) != 0)
            continue;
        size_t name_length = strlen(entry->d_name);
        if (prefix_length + name_length + 1 >= PATH_MAX)
            continue;
        memcpy(prefix + prefix_length, entry->d_name, name_length + 1);
        // A match must be a directory to go on below it.
        if (*rest) {
            struct stat information;
            if (stat(prefix, &information) < 0 || !S_ISDIR(information.st_mode))
                continue;
        }
        result = expand(prefix, prefix_length + name_length, rest, flags, error_function, results);
        if (result)
            break;
    }
    closedir(directory);
    prefix[prefix_length] = '\0';
    return result;
}

int compare_paths(void const* a, void const* b)
{
    return strcmp(*static_cast<char* const*>(a), *static_cast<char* const*>(b));
}

}

extern "C" {

int fnmatch(char const* pattern, char const* string, int flags)
{
    return match(pattern, string, flags, true) ? 0 : FNM_NOMATCH;
}

int glob(char const* pattern, int flags, int (*error_function)(char const*, int), glob_t* result)
{
    Results results;
    size_t offsets = (flags & GLOB_DOOFFS) ? result->gl_offs : 0;
    if (flags & GLOB_APPEND) {
        results.paths = result->gl_pathv;
        results.count = result->gl_pathc + offsets;
        results.capacity = results.count + 1;
    } else {
        for (size_t i = 0; i < offsets; ++i)
            results.add("", 0);
        for (size_t i = 0; i < offsets; ++i) {
            free(results.paths[i]);
            results.paths[i] = nullptr;
        }
    }
    size_t first_new = results.count;

    // ~ and ~user at the start.
    char expanded[PATH_MAX];
    if ((flags & GLOB_TILDE) && pattern[0] == '~') {
        char const* slash = strchr(pattern, '/');
        size_t name_length = slash ? static_cast<size_t>(slash - pattern - 1) : strlen(pattern) - 1;
        char const* home = nullptr;
        if (name_length == 0) {
            home = getenv("HOME");
        } else {
            char name[64];
            if (name_length < sizeof(name)) {
                memcpy(name, pattern + 1, name_length);
                name[name_length] = '\0';
                if (struct passwd* user = getpwnam(name))
                    home = user->pw_dir;
            }
        }
        if (home && strlen(home) + strlen(pattern) < sizeof(expanded)) {
            strcpy(expanded, home);
            strcat(expanded, slash ? slash : "");
            pattern = expanded;
        }
    }

    char prefix[PATH_MAX];
    int error = expand(prefix, 0, pattern, flags, error_function, results);
    if (error) {
        result->gl_pathv = results.paths;
        result->gl_pathc = results.count - offsets;
        return error;
    }
    if (results.count == first_new) {
        if (!(flags & GLOB_NOCHECK)) {
            result->gl_pathv = results.paths;
            result->gl_pathc = results.count - offsets;
            if (results.paths)
                results.paths[results.count] = nullptr;
            return GLOB_NOMATCH;
        }
        if (!results.add(pattern, strlen(pattern)))
            return GLOB_NOSPACE;
    }
    if (!(flags & GLOB_NOSORT))
        qsort(results.paths + first_new, results.count - first_new, sizeof(char*), compare_paths);
    results.paths[results.count] = nullptr;
    result->gl_pathv = results.paths;
    result->gl_pathc = results.count - offsets;
    return 0;
}

void globfree(glob_t* result)
{
    if (!result->gl_pathv)
        return;
    for (size_t i = 0; i < result->gl_pathc + result->gl_offs; ++i)
        free(result->gl_pathv[i]);
    free(result->gl_pathv);
    result->gl_pathv = nullptr;
    result->gl_pathc = 0;
}
}
