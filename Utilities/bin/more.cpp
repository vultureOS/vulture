/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// more / less [file...]: a screen at a time. Space: next screen; Return
// or j: next line; b: back a screen; k: back a line; g/G: start/end;
// /pattern: search; q: quit.
namespace {

char** s_lines;
size_t s_count;

void load(FILE* file)
{
    size_t capacity = 1024;
    s_lines = static_cast<char**>(malloc(capacity * sizeof(char*)));
    char* line = nullptr;
    size_t size = 0;
    ssize_t length;
    while ((length = getline(&line, &size, file)) >= 0) {
        if (length > 0 && line[length - 1] == '\n')
            line[length - 1] = '\0';
        if (s_count == capacity) {
            capacity *= 2;
            s_lines = static_cast<char**>(realloc(s_lines, capacity * sizeof(char*)));
        }
        s_lines[s_count++] = strdup(line);
    }
    free(line);
}

}

int main(int argc, char** argv)
{
    FILE* file = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!file) {
        warn(last_component(argv[0]), argv[1]);
        return 1;
    }
    load(file);
    if (!isatty(STDOUT_FILENO)) {
        for (size_t i = 0; i < s_count; ++i)
            printf("%s\n", s_lines[i]);
        return 0;
    }
    // Keys come from the terminal even when the text came from a pipe.
    int keyboard = open("/dev/tty", O_RDONLY);
    if (keyboard < 0)
        keyboard = STDIN_FILENO;
    struct termios original, raw;
    tcgetattr(keyboard, &original);
    raw = original;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    tcsetattr(keyboard, TCSADRAIN, &raw);
    struct winsize size { 25, 80, 0, 0 };
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    size_t page = size.ws_row > 1 ? size.ws_row - 1u : 24u;
    size_t top = 0;
    auto draw = [&]() {
        printf("\x1b[H\x1b[2J");
        for (size_t i = top; i < top + page && i < s_count; ++i)
            printf("%.*s\n", static_cast<int>(size.ws_col), s_lines[i]);
        int percent = s_count ? static_cast<int>(min(top + page, s_count) * 100 / s_count) : 100;
        printf(top + page >= s_count ? "\x1b[7m(END)\x1b[0m" : "\x1b[7m%s %d%%\x1b[0m", argc > 1 ? argv[1] : "", percent);
        fflush(stdout);
    };
    draw();
    for (;;) {
        char key;
        if (read(keyboard, &key, 1) != 1 || key == 'q' || key == 'Q')
            break;
        switch (key) {
        case ' ':
        case 'f':
            if (top + page < s_count)
                top += page;
            break;
        case '\n':
        case '\r':
        case 'j':
            if (top + page < s_count)
                ++top;
            break;
        case 'b':
            top = top > page ? top - page : 0;
            break;
        case 'k':
            if (top > 0)
                --top;
            break;
        case 'g':
            top = 0;
            break;
        case 'G':
            top = s_count > page ? s_count - page : 0;
            break;
        case '/': {
            printf("\r\x1b[K/");
            fflush(stdout);
            tcsetattr(keyboard, TCSADRAIN, &original);
            char pattern[256] = {};
            size_t length = 0;
            char c;
            while (length < sizeof(pattern) - 1 && read(keyboard, &c, 1) == 1 && c != '\n')
                pattern[length++] = c;
            tcsetattr(keyboard, TCSADRAIN, &raw);
            for (size_t i = top + 1; i < s_count; ++i) {
                if (strstr(s_lines[i], pattern)) {
                    top = i;
                    break;
                }
            }
            break;
        }
        default:
            continue;
        }
        draw();
    }
    tcsetattr(keyboard, TCSADRAIN, &original);
    printf("\r\x1b[K");
    return 0;
}
