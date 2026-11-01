/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <ctype.h>
#include <sys/wait.h>
#include <unistd.h>

// xargs [-0] [-n count] [-I replace] [-t] [command [argument...]]
namespace {

int run(char** argv)
{
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "xargs: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 255)
        exit(124);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 125;
}

}

int main(int argc, char** argv)
{
    bool null_separated = false, trace = false;
    long per_command = 4096;
    char const* replace = nullptr;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        char const* option = argv[first];
        auto value = [&]() -> char const* { return option[2] ? option + 2 : (first + 1 < argc ? argv[++first] : nullptr); };
        if (strcmp(option, "-0") == 0) {
            null_separated = true;
        } else if (strcmp(option, "-t") == 0) {
            trace = true;
        } else if (option[1] == 'n' && (option[2] || first + 1 < argc)) {
            per_command = atol(value());
            if (per_command < 1)
                per_command = 1;
        } else if (option[1] == 'I' && (option[2] || first + 1 < argc)) {
            replace = value();
            per_command = 1;
        } else {
            fprintf(stderr, "usage: xargs [-0t] [-I replstr] [-n number] [utility [argument ...]]\n");
            return 1;
        }
    }
    char* default_command[] = { const_cast<char*>("echo"), nullptr };
    char** command = first < argc ? argv + first : default_command;
    int command_count = first < argc ? argc - first : 1;

    // Read the items.
    size_t capacity = 256, count = 0;
    auto** items = static_cast<char**>(malloc(capacity * sizeof(char*)));
    char word[4096];
    size_t length = 0;
    int c;
    bool in_quote = false;
    char quote = 0;
    auto finish = [&]() {
        if (length == 0 && !null_separated)
            return;
        word[length] = '\0';
        if (count == capacity) {
            capacity *= 2;
            items = static_cast<char**>(realloc(items, capacity * sizeof(char*)));
        }
        items[count++] = strdup(word);
        length = 0;
    };
    while ((c = getchar()) != EOF) {
        if (null_separated) {
            if (c == '\0')
                finish();
            else if (length < sizeof(word) - 1)
                word[length++] = static_cast<char>(c);
            continue;
        }
        if (replace) {
            if (c == '\n')
                finish();
            else if (length < sizeof(word) - 1)
                word[length++] = static_cast<char>(c);
            continue;
        }
        if (in_quote) {
            if (c == quote)
                in_quote = false;
            else if (length < sizeof(word) - 1)
                word[length++] = static_cast<char>(c);
            continue;
        }
        if (c == '"' || c == '\'') {
            in_quote = true;
            quote = static_cast<char>(c);
        } else if (c == '\\') {
            int next = getchar();
            if (next != EOF && length < sizeof(word) - 1)
                word[length++] = static_cast<char>(next);
        } else if (isspace(c)) {
            finish();
        } else if (length < sizeof(word) - 1) {
            word[length++] = static_cast<char>(c);
        }
    }
    if (length > 0)
        finish();

    int status = 0;
    size_t done = 0;
    do {
        size_t batch = count - done < static_cast<size_t>(per_command) ? count - done : static_cast<size_t>(per_command);
        auto** arguments = static_cast<char**>(calloc(static_cast<size_t>(command_count) + batch + 1, sizeof(char*)));
        int position = 0;
        for (int i = 0; i < command_count; ++i) {
            if (replace && batch > 0 && strstr(command[i], replace)) {
                // Substitute every occurrence.
                char built[8192];
                size_t built_length = 0;
                char const* text = command[i];
                size_t replace_length = strlen(replace);
                while (*text && built_length < sizeof(built) - 1) {
                    if (strncmp(text, replace, replace_length) == 0) {
                        built_length += strlcpy(built + built_length, items[done], sizeof(built) - built_length);
                        text += replace_length;
                    } else {
                        built[built_length++] = *text++;
                    }
                }
                built[built_length] = '\0';
                arguments[position++] = strdup(built);
            } else {
                arguments[position++] = command[i];
            }
        }
        if (!replace) {
            for (size_t i = 0; i < batch; ++i)
                arguments[position++] = items[done + i];
        }
        if (trace) {
            for (int i = 0; i < position; ++i)
                fprintf(stderr, i ? " %s" : "%s", arguments[i]);
            fputc('\n', stderr);
        }
        if (batch > 0 || count == 0) {
            int result = run(arguments);
            if (result != 0)
                status = result == 127 ? 127 : 123;
        }
        free(arguments);
        done += batch;
    } while (done < count);
    return status;
}
