/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FloatText.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {

extern char** environ;

// --- The environment ---
//
// environ starts out pointing at the strings the kernel put on the stack.
// The first change copies the array to the heap; strings set here are
// allocated and never freed (other code may still hold them, as POSIX
// allows).

static bool s_environment_owned = false;

static size_t environment_size()
{
    size_t count = 0;
    while (environ && environ[count])
        ++count;
    return count;
}

static bool own_environment(size_t extra)
{
    size_t count = environment_size();
    if (s_environment_owned && extra == 0)
        return true;
    auto** copy = static_cast<char**>(malloc((count + extra + 1) * sizeof(char*)));
    if (!copy)
        return false;
    for (size_t i = 0; i < count; ++i)
        copy[i] = environ[i];
    copy[count] = nullptr;
    if (s_environment_owned)
        free(environ);
    environ = copy;
    s_environment_owned = true;
    return true;
}

static int find_variable(char const* name, size_t length)
{
    for (size_t i = 0; environ && environ[i]; ++i) {
        if (strncmp(environ[i], name, length) == 0 && environ[i][length] == '=')
            return static_cast<int>(i);
    }
    return -1;
}

int putenv(char* entry)
{
    char const* equals = strchr(entry, '=');
    if (!equals) {
        errno = EINVAL;
        return -1;
    }
    int index = find_variable(entry, static_cast<size_t>(equals - entry));
    if (index >= 0) {
        if (!own_environment(0)) {
            errno = ENOMEM;
            return -1;
        }
        environ[index] = entry;
        return 0;
    }
    size_t count = environment_size();
    if (!own_environment(1)) {
        errno = ENOMEM;
        return -1;
    }
    environ[count] = entry;
    environ[count + 1] = nullptr;
    return 0;
}

int setenv(char const* name, char const* value, int overwrite)
{
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && getenv(name))
        return 0;
    size_t name_length = strlen(name);
    size_t value_length = strlen(value);
    auto* entry = static_cast<char*>(malloc(name_length + value_length + 2));
    if (!entry) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(entry, name, name_length);
    entry[name_length] = '=';
    memcpy(entry + name_length + 1, value, value_length + 1);
    return putenv(entry);
}

int unsetenv(char const* name)
{
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    int index = find_variable(name, strlen(name));
    if (index < 0)
        return 0;
    if (!own_environment(0)) {
        errno = ENOMEM;
        return -1;
    }
    for (size_t i = static_cast<size_t>(index); environ[i]; ++i)
        environ[i] = environ[i + 1];
    return 0;
}

int clearenv(void)
{
    if (!own_environment(0))
        return -1;
    environ[0] = nullptr;
    return 0;
}

// --- Paths and temporary files ---

char* realpath(char const* path, char* resolved)
{
    if (!path) {
        errno = EINVAL;
        return nullptr;
    }
    // Absolute first, then walked component by component, following links.
    char work[PATH_MAX * 2];
    if (path[0] == '/') {
        if (strlen(path) >= sizeof(work)) {
            errno = ENAMETOOLONG;
            return nullptr;
        }
        strcpy(work, path);
    } else {
        if (!getcwd(work, PATH_MAX))
            return nullptr;
        size_t length = strlen(work);
        if (length + 1 + strlen(path) >= sizeof(work)) {
            errno = ENAMETOOLONG;
            return nullptr;
        }
        work[length] = '/';
        strcpy(work + length + 1, path);
    }
    char result[PATH_MAX];
    size_t result_length = 0;
    result[0] = '\0';
    int links = 0;
    char* cursor = work;
    while (*cursor) {
        while (*cursor == '/')
            ++cursor;
        if (!*cursor)
            break;
        char* end = cursor;
        while (*end && *end != '/')
            ++end;
        size_t length = static_cast<size_t>(end - cursor);
        if (length == 1 && cursor[0] == '.') {
            cursor = end;
            continue;
        }
        if (length == 2 && cursor[0] == '.' && cursor[1] == '.') {
            while (result_length > 0 && result[result_length - 1] != '/')
                --result_length;
            if (result_length > 0)
                --result_length;
            result[result_length] = '\0';
            cursor = end;
            continue;
        }
        if (result_length + 1 + length >= sizeof(result)) {
            errno = ENAMETOOLONG;
            return nullptr;
        }
        result[result_length++] = '/';
        memcpy(result + result_length, cursor, length);
        result_length += length;
        result[result_length] = '\0';
        struct stat information;
        if (lstat(result, &information) < 0)
            return nullptr;
        if (S_ISLNK(information.st_mode)) {
            if (++links > 40) {
                errno = ELOOP;
                return nullptr;
            }
            char target[PATH_MAX];
            ssize_t target_length = readlink(result, target, sizeof(target) - 1);
            if (target_length < 0)
                return nullptr;
            target[target_length] = '\0';
            // The rest of the path goes after the link's target.
            char rest[PATH_MAX * 2];
            size_t rest_length = strlen(end);
            if (static_cast<size_t>(target_length) + rest_length + 2 >= sizeof(rest)) {
                errno = ENAMETOOLONG;
                return nullptr;
            }
            memcpy(rest, target, static_cast<size_t>(target_length));
            memcpy(rest + target_length, end, rest_length + 1);
            if (target[0] == '/') {
                result_length = 0;
            } else {
                while (result_length > 0 && result[result_length - 1] != '/')
                    --result_length;
                if (result_length > 0)
                    --result_length;
            }
            result[result_length] = '\0';
            strcpy(work, rest);
            cursor = work;
            continue;
        }
        cursor = end;
    }
    if (result_length == 0) {
        result[0] = '/';
        result[1] = '\0';
    }
    if (!resolved) {
        resolved = static_cast<char*>(malloc(strlen(result) + 1));
        if (!resolved) {
            errno = ENOMEM;
            return nullptr;
        }
    }
    strcpy(resolved, result);
    return resolved;
}

static bool fill_template(char* template_string, size_t suffix_length)
{
    size_t length = strlen(template_string);
    if (length < 6 + suffix_length || strncmp(template_string + length - suffix_length - 6, "XXXXXX", 6) != 0) {
        errno = EINVAL;
        return false;
    }
    static char const alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (size_t i = length - suffix_length - 6; i < length - suffix_length; ++i)
        template_string[i] = alphabet[arc4random_uniform(sizeof(alphabet) - 1)];
    return true;
}

int mkstemps(char* template_string, int suffix_length)
{
    char saved[PATH_MAX];
    strncpy(saved, template_string, sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';
    for (int attempt = 0; attempt < 100; ++attempt) {
        strcpy(template_string, saved);
        if (!fill_template(template_string, static_cast<size_t>(suffix_length)))
            return -1;
        int fd = open(template_string, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    errno = EEXIST;
    return -1;
}

int mkstemp(char* template_string)
{
    return mkstemps(template_string, 0);
}

char* mkdtemp(char* template_string)
{
    char saved[PATH_MAX];
    strncpy(saved, template_string, sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';
    for (int attempt = 0; attempt < 100; ++attempt) {
        strcpy(template_string, saved);
        if (!fill_template(template_string, 0))
            return nullptr;
        if (mkdir(template_string, 0700) == 0)
            return template_string;
        if (errno != EEXIST)
            return nullptr;
    }
    return nullptr;
}

// --- Running commands ---

int system(char const* command)
{
    if (!command)
        return access("/bin/sh", X_OK) == 0;
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        char* argv[] = { const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(command), nullptr };
        execv("/bin/sh", argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
}

// --- Numbers ---

static double parse_number(char const* string, char** end, int mantissa_bits)
{
    char const* cursor = string;
    while (isspace(static_cast<unsigned char>(*cursor)))
        ++cursor;
    bool negative = false;
    if (*cursor == '+' || *cursor == '-')
        negative = *cursor++ == '-';
    auto finish = [&](double value, char const* stop) {
        if (end)
            *end = const_cast<char*>(stop);
        return negative ? -value : value;
    };
    if (strncasecmp(cursor, "inf", 3) == 0)
        return finish(__builtin_inf(), cursor + (strncasecmp(cursor, "infinity", 8) == 0 ? 8 : 3));
    if (strncasecmp(cursor, "nan", 3) == 0) {
        char const* after = cursor + 3;
        // nan(whatever)
        if (*after == '(') {
            char const* closing = after + 1;
            while (isalnum(static_cast<unsigned char>(*closing)) || *closing == '_')
                ++closing;
            if (*closing == ')')
                after = closing + 1;
        }
        return finish(__builtin_nan(""), after);
    }
    bool out_of_range = false;
    char const* stop = nullptr;
    double const value = LibC::parse_float(cursor, &stop, mantissa_bits, out_of_range);
    if (stop == cursor) {
        // Not a number at all.
        if (end)
            *end = const_cast<char*>(string);
        return 0;
    }
    if (out_of_range)
        errno = ERANGE;
    return finish(value, stop);
}

double strtod(char const* string, char** end)
{
    return parse_number(string, end, 53);
}

float strtof(char const* string, char** end)
{
    return static_cast<float>(parse_number(string, end, 24));
}
double atof(char const* string) { return strtod(string, nullptr); }

void* bsearch(void const* key, void const* base, size_t count, size_t size, int (*compare)(void const*, void const*))
{
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        void const* element = static_cast<char const*>(base) + middle * size;
        int order = compare(key, element);
        if (order == 0)
            return const_cast<void*>(element);
        if (order < 0)
            high = middle;
        else
            low = middle + 1;
    }
    return nullptr;
}

div_t div(int numerator, int denominator) { return { numerator / denominator, numerator % denominator }; }
ldiv_t ldiv(long numerator, long denominator) { return { numerator / denominator, numerator % denominator }; }
lldiv_t lldiv(long long numerator, long long denominator) { return { numerator / denominator, numerator % denominator }; }

static char const* s_program_name = "";

char const* getprogname(void)
{
    return s_program_name;
}

void setprogname(char const* name)
{
    char const* slash = strrchr(name, '/');
    s_program_name = slash ? slash + 1 : name;
}
}
