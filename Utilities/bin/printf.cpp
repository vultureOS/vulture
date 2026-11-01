/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// printf FORMAT [argument...]: the format is reused until the arguments
// are used up. Escapes \n \t \\ \0NNN \xHH; conversions %d %i %o %u %x %X
// %c %s %b %f %e %g %%, with flags, width and precision.

static int s_status = 0;

static bool print_escape(char const*& cursor, bool in_b)
{
    char c = *++cursor;
    switch (c) {
    case 'n':
        putchar('\n');
        break;
    case 't':
        putchar('\t');
        break;
    case 'r':
        putchar('\r');
        break;
    case 'a':
        putchar('\a');
        break;
    case 'b':
        putchar('\b');
        break;
    case 'f':
        putchar('\f');
        break;
    case 'v':
        putchar('\v');
        break;
    case 'e':
        putchar('\x1b');
        break;
    case '\\':
        putchar('\\');
        break;
    case 'c':
        if (in_b) {
            fflush(stdout);
            exit(s_status);
        }
        putchar('\\');
        putchar('c');
        break;
    case 'x': {
        int value = 0, digits = 0;
        while (digits < 2 && isxdigit(static_cast<unsigned char>(cursor[1]))) {
            char d = *++cursor;
            value = value * 16 + (isdigit(static_cast<unsigned char>(d)) ? d - '0' : tolower(d) - 'a' + 10);
            ++digits;
        }
        putchar(value);
        break;
    }
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7': {
        int value = c - '0', digits = 1;
        int limit = (in_b && c == '0') ? 4 : 3;
        while (digits < limit && cursor[1] >= '0' && cursor[1] <= '7') {
            value = value * 8 + (*++cursor - '0');
            ++digits;
        }
        putchar(value & 0xff);
        break;
    }
    case '\0':
        putchar('\\');
        --cursor;
        return false;
    default:
        putchar('\\');
        putchar(c);
    }
    return true;
}

static long long integer_argument(char const* text)
{
    if (!text)
        return 0;
    if (text[0] == '\'' || text[0] == '"')
        return static_cast<unsigned char>(text[1]);
    char* end;
    long long value = strtoll(text, &end, 0);
    if (*end || end == text) {
        fprintf(stderr, "printf: %s: invalid number\n", text);
        s_status = 1;
    }
    return value;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: printf format [arguments ...]\n");
        return 1;
    }
    char const* format = argv[1];
    int next = 2;
    do {
        bool consumed = false;
        for (char const* cursor = format; *cursor; ++cursor) {
            if (*cursor == '\\') {
                print_escape(cursor, false);
                continue;
            }
            if (*cursor != '%') {
                putchar(*cursor);
                continue;
            }
            if (cursor[1] == '%') {
                putchar('%');
                ++cursor;
                continue;
            }
            // Copy the specification, resolving '*' from the arguments.
            char spec[64] = "%";
            size_t length = 1;
            ++cursor;
            while (*cursor && strchr("-+ #0", *cursor) && length < 20)
                spec[length++] = *cursor++;
            for (int part = 0; part < 2; ++part) {
                if (*cursor == '*') {
                    length += static_cast<size_t>(snprintf(spec + length, sizeof(spec) - length, "%d", static_cast<int>(integer_argument(next < argc ? argv[next] : nullptr))));
                    if (next < argc)
                        ++next;
                    consumed = true;
                    ++cursor;
                }
                while (isdigit(static_cast<unsigned char>(*cursor)) && length < 40)
                    spec[length++] = *cursor++;
                if (part == 0 && *cursor == '.')
                    spec[length++] = *cursor++;
                else
                    break;
            }
            char conversion = *cursor;
            if (!conversion)
                break;
            char const* argument = next < argc ? argv[next] : nullptr;
            if (argument) {
                ++next;
                consumed = true;
            }
            switch (conversion) {
            case 'd':
            case 'i':
                spec[length++] = 'l';
                spec[length++] = 'l';
                spec[length++] = 'd';
                spec[length] = '\0';
                printf(spec, integer_argument(argument));
                break;
            case 'o':
            case 'u':
            case 'x':
            case 'X':
                spec[length++] = 'l';
                spec[length++] = 'l';
                spec[length++] = conversion;
                spec[length] = '\0';
                printf(spec, static_cast<unsigned long long>(integer_argument(argument)));
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
                spec[length++] = conversion;
                spec[length] = '\0';
                printf(spec, argument ? strtod(argument, nullptr) : 0.0);
                break;
            case 'c':
                spec[length++] = 'c';
                spec[length] = '\0';
                printf(spec, argument ? argument[0] : 0);
                break;
            case 's':
                spec[length++] = 's';
                spec[length] = '\0';
                printf(spec, argument ? argument : "");
                break;
            case 'b':
                for (char const* text = argument ? argument : ""; *text; ++text) {
                    if (*text == '\\')
                        print_escape(text, true);
                    else
                        putchar(*text);
                }
                break;
            default:
                fprintf(stderr, "printf: %%%c: invalid conversion\n", conversion);
                return 1;
            }
        }
        if (!consumed)
            break;
    } while (next < argc);
    fflush(stdout);
    return s_status;
}
