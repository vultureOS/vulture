/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibSystem/Syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sandbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Putting a profile together: "include NAME" brings in that file; ${HOME} and ${PROGRAM} are who is
// asking and what they are about to run. What reaches the kernel is plain rules.

namespace {

constexpr size_t largest_profile = 64 * 1024;
constexpr int deepest_include = 8;

struct Builder {
    char* text;
    size_t length;
    char const* home;
    char const* program;
    bool failed;
};

void append(Builder& builder, char const* characters, size_t count)
{
    if (builder.length + count + 1 > largest_profile) {
        builder.failed = true;
        return;
    }
    memcpy(builder.text + builder.length, characters, count);
    builder.length += count;
    builder.text[builder.length] = '\0';
}

bool add_text(Builder&, char const* text, size_t size, int depth);

bool add_named(Builder& builder, char const* name, int depth)
{
    if (depth > deepest_include || strchr(name, '/'))
        return false;
    char path[256];
    snprintf(path, sizeof(path), "/System/Library/Sandbox/Profiles/%s.sb", name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    auto* contents = static_cast<char*>(malloc(largest_profile));
    if (!contents) {
        close(fd);
        return false;
    }
    size_t size = 0;
    for (;;) {
        ssize_t got = read(fd, contents + size, largest_profile - size);
        if (got <= 0)
            break;
        size += static_cast<size_t>(got);
    }
    close(fd);
    bool const ok = add_text(builder, contents, size, depth + 1);
    free(contents);
    return ok;
}

bool add_text(Builder& builder, char const* text, size_t size, int depth)
{
    size_t at = 0;
    while (at < size) {
        size_t end = at;
        while (end < size && text[end] != '\n')
            ++end;
        size_t const line_length = end - at;
        char const* line = text + at;
        at = end + 1;
        if (line_length == 0 || line[0] == '#')
            continue;
        if (line_length > 8 && strncmp(line, "include ", 8) == 0) {
            char name[128];
            size_t const name_length = line_length - 8;
            if (name_length >= sizeof(name))
                return false;
            memcpy(name, line + 8, name_length);
            name[name_length] = '\0';
            if (!add_named(builder, name, depth))
                return false;
            continue;
        }
        // A line about ${PROGRAM}, with none to say, says nothing.
        char expanded[1024];
        size_t written = 0;
        bool skip = false;
        for (size_t i = 0; i < line_length; ++i) {
            char const* value = nullptr;
            size_t variable = 0;
            if (line_length - i >= 7 && strncmp(line + i, "${HOME}", 7) == 0) {
                value = builder.home;
                variable = 7;
            } else if (line_length - i >= 10 && strncmp(line + i, "${PROGRAM}", 10) == 0) {
                value = builder.program;
                variable = 10;
                if (!value) {
                    skip = true;
                    break;
                }
            }
            if (value) {
                size_t const value_length = strlen(value);
                if (written + value_length >= sizeof(expanded))
                    return false;
                memcpy(expanded + written, value, value_length);
                written += value_length;
                i += variable - 1;
                continue;
            }
            if (written + 1 >= sizeof(expanded))
                return false;
            expanded[written++] = line[i];
        }
        if (skip)
            continue;
        append(builder, expanded, written);
        append(builder, "\n", 1);
    }
    return !builder.failed;
}

void set_error(char** error, char const* message)
{
    if (error)
        *error = strdup(message);
}

}

extern "C" {

int sandbox_init_for_program(char const* profile, unsigned long long flags, char const* program, char** error)
{
    auto* text = static_cast<char*>(malloc(largest_profile));
    if (!text) {
        set_error(error, "out of memory");
        errno = ENOMEM;
        return -1;
    }
    text[0] = '\0';
    char const* home = getenv("HOME");
    if (auto* entry = getpwuid(getuid()))
        home = entry->pw_dir;
    Builder builder { text, 0, home ? home : "/var/empty", program, false };
    // What the log calls it.
    if (flags & SANDBOX_NAMED) {
        char first[160];
        snprintf(first, sizeof(first), "name %s\n", profile);
        append(builder, first, strlen(first));
    } else {
        append(builder, "name custom\n", 12);
    }
    bool const ok = (flags & SANDBOX_NAMED) ? add_named(builder, profile, 0) : add_text(builder, profile, strlen(profile), 0);
    if (!ok) {
        free(text);
        set_error(error, "no such profile, or a bad one");
        errno = EINVAL;
        return -1;
    }
    long const result = System::syscall(System::Syscall::sandbox_apply, reinterpret_cast<unsigned long>(text), builder.length);
    free(text);
    if (result < 0) {
        errno = static_cast<int>(-result);
        set_error(error, strerror(errno));
        return -1;
    }
    return 0;
}

int sandbox_init(char const* profile, unsigned long long flags, char** error)
{
    return sandbox_init_for_program(profile, flags, nullptr, error);
}

void sandbox_free_error(char* error)
{
    free(error);
}

int sandbox_check_service(pid_t pid, char const* service)
{
    long const result = System::syscall(System::Syscall::sandbox_check, static_cast<unsigned long>(pid), reinterpret_cast<unsigned long>(service), strlen(service));
    if (result < 0) {
        errno = static_cast<int>(-result);
        return -1;
    }
    return 0;
}
}
