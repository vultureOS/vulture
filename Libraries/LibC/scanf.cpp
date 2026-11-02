/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The scanf family: integers (%d %i %u %o %x %X %p), floating point
// (%f %e %g %a), characters (%c), strings (%s), scansets (%[...]), %n and
// %%; field widths, assignment suppression (*) and the hh, h, l, ll, j,
// z, t and L length modifiers.

namespace {

struct Source {
    FILE* file { nullptr };
    char const* string { nullptr };
    size_t consumed { 0 };

    int get()
    {
        int character;
        if (file) {
            character = fgetc(file);
        } else {
            character = *string ? static_cast<unsigned char>(*string) : EOF;
            if (character != EOF)
                ++string;
        }
        if (character != EOF)
            ++consumed;
        return character;
    }

    void unget(int character)
    {
        if (character == EOF)
            return;
        --consumed;
        if (file)
            ungetc(character, file);
        else
            --string;
    }
};

enum class Length {
    Char,
    Short,
    Int,
    Long,
    LongLong,
    Size,
};

void store_signed(va_list& arguments, Length length, long long value)
{
    switch (length) {
    case Length::Char:
        *va_arg(arguments, signed char*) = static_cast<signed char>(value);
        break;
    case Length::Short:
        *va_arg(arguments, short*) = static_cast<short>(value);
        break;
    case Length::Int:
        *va_arg(arguments, int*) = static_cast<int>(value);
        break;
    case Length::Long:
        *va_arg(arguments, long*) = static_cast<long>(value);
        break;
    case Length::LongLong:
        *va_arg(arguments, long long*) = value;
        break;
    case Length::Size:
        *va_arg(arguments, ssize_t*) = static_cast<ssize_t>(value);
        break;
    }
}

void store_unsigned(va_list& arguments, Length length, unsigned long long value)
{
    switch (length) {
    case Length::Char:
        *va_arg(arguments, unsigned char*) = static_cast<unsigned char>(value);
        break;
    case Length::Short:
        *va_arg(arguments, unsigned short*) = static_cast<unsigned short>(value);
        break;
    case Length::Int:
        *va_arg(arguments, unsigned*) = static_cast<unsigned>(value);
        break;
    case Length::Long:
        *va_arg(arguments, unsigned long*) = static_cast<unsigned long>(value);
        break;
    case Length::LongLong:
        *va_arg(arguments, unsigned long long*) = value;
        break;
    case Length::Size:
        *va_arg(arguments, size_t*) = static_cast<size_t>(value);
        break;
    }
}

int digit_value(int character, int base)
{
    int value;
    if (character >= '0' && character <= '9')
        value = character - '0';
    else if (character >= 'a' && character <= 'z')
        value = character - 'a' + 10;
    else if (character >= 'A' && character <= 'Z')
        value = character - 'A' + 10;
    else
        return -1;
    return value < base ? value : -1;
}

int scan(Source& source, char const* format, va_list given_arguments)
{
    // (On some architectures, what was passed here isn't of the type it says it is.)
    va_list arguments;
    va_copy(arguments, given_arguments);
    int assigned = 0;
    bool any_input = false;
    for (char const* cursor = format; *cursor; ++cursor) {
        if (isspace(static_cast<unsigned char>(*cursor))) {
            int character;
            while ((character = source.get()) != EOF && isspace(character)) {
            }
            source.unget(character);
            continue;
        }
        if (*cursor != '%' || cursor[1] == '%') {
            if (*cursor == '%')
                ++cursor;
            int character = source.get();
            if (character == EOF)
                return any_input || assigned ? assigned : EOF;
            if (character != static_cast<unsigned char>(*cursor)) {
                source.unget(character);
                return assigned;
            }
            continue;
        }
        ++cursor;
        bool suppress = false;
        if (*cursor == '*') {
            suppress = true;
            ++cursor;
        }
        size_t width = 0;
        while (*cursor >= '0' && *cursor <= '9')
            width = width * 10 + static_cast<size_t>(*cursor++ - '0');
        Length length = Length::Int;
        if (cursor[0] == 'h' && cursor[1] == 'h') {
            length = Length::Char;
            cursor += 2;
        } else if (*cursor == 'h') {
            length = Length::Short;
            ++cursor;
        } else if (cursor[0] == 'l' && cursor[1] == 'l') {
            length = Length::LongLong;
            cursor += 2;
        } else if (*cursor == 'l' || *cursor == 'L' || *cursor == 'j' || *cursor == 'q') {
            length = *cursor == 'l' ? Length::Long : Length::LongLong;
            ++cursor;
        } else if (*cursor == 'z' || *cursor == 't') {
            length = Length::Size;
            ++cursor;
        }
        char conversion = *cursor;
        if (conversion == '\0')
            break;
        if (conversion == 'n') {
            if (!suppress)
                store_signed(arguments, length, static_cast<long long>(source.consumed));
            continue;
        }
        // All but %c and %[ skip leading white space.
        if (conversion != 'c' && conversion != '[') {
            int character;
            while ((character = source.get()) != EOF && isspace(character)) {
            }
            source.unget(character);
        }
        if (width == 0)
            width = conversion == 'c' ? 1 : SIZE_MAX;

        switch (conversion) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            int base = conversion == 'o' ? 8 : (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16
                                                                                                             : (conversion == 'i' ? 0 : 10);
            size_t taken = 0;
            int character = source.get();
            bool negative = false;
            if ((character == '-' || character == '+') && taken < width) {
                negative = character == '-';
                ++taken;
                character = source.get();
            }
            if ((base == 0 || base == 16) && character == '0' && taken < width) {
                ++taken;
                int next = source.get();
                if ((next == 'x' || next == 'X') && taken < width) {
                    base = 16;
                    ++taken;
                    character = source.get();
                } else {
                    source.unget(next);
                    if (base == 0)
                        base = 8;
                    // The 0 itself is a digit.
                    character = '0';
                    --taken;
                }
            }
            if (base == 0)
                base = 10;
            unsigned long long value = 0;
            size_t digits = 0;
            while (character != EOF && taken < width && digit_value(character, base) >= 0) {
                value = value * static_cast<unsigned long long>(base) + static_cast<unsigned long long>(digit_value(character, base));
                ++digits;
                ++taken;
                character = source.get();
            }
            source.unget(character);
            if (digits == 0)
                return any_input || assigned ? assigned : (character == EOF ? EOF : assigned);
            any_input = true;
            if (!suppress) {
                if (conversion == 'p')
                    *va_arg(arguments, void**) = reinterpret_cast<void*>(static_cast<uintptr_t>(value));
                else if (conversion == 'd' || conversion == 'i')
                    store_signed(arguments, length, negative ? -static_cast<long long>(value) : static_cast<long long>(value));
                else
                    store_unsigned(arguments, length, negative ? 0 - value : value);
                ++assigned;
            }
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            char text[128];
            size_t used = 0;
            int character = source.get();
            while (character != EOF && used < sizeof(text) - 1 && used < width
                && (isdigit(character) || character == '.' || character == 'e' || character == 'E' || character == '-' || character == '+'
                    || character == 'x' || character == 'X' || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F')
                    || character == 'i' || character == 'n' || character == 'I' || character == 'N')) {
                text[used++] = static_cast<char>(character);
                character = source.get();
            }
            source.unget(character);
            text[used] = '\0';
            char* end;
            double value = strtod(text, &end);
            // Give back what strtod did not want.
            for (char* back = text + used; back > end;)
                source.unget(static_cast<unsigned char>(*--back));
            if (end == text)
                return any_input || assigned ? assigned : (character == EOF ? EOF : assigned);
            any_input = true;
            if (!suppress) {
                if (length == Length::Long || length == Length::LongLong)
                    *va_arg(arguments, double*) = value;
                else
                    *va_arg(arguments, float*) = static_cast<float>(value);
                ++assigned;
            }
            break;
        }
        case 'c': {
            char* destination = suppress ? nullptr : va_arg(arguments, char*);
            for (size_t i = 0; i < width; ++i) {
                int character = source.get();
                if (character == EOF)
                    return any_input || assigned ? assigned : EOF;
                if (destination)
                    destination[i] = static_cast<char>(character);
            }
            any_input = true;
            if (!suppress)
                ++assigned;
            break;
        }
        case 's':
        case '[': {
            bool set[256] {};
            bool scanset = conversion == '[';
            if (scanset) {
                ++cursor;
                bool negate = *cursor == '^';
                if (negate)
                    ++cursor;
                bool members[256] {};
                if (*cursor == ']')
                    members[static_cast<unsigned char>(*cursor++)] = true;
                while (*cursor && *cursor != ']') {
                    if (cursor[1] == '-' && cursor[2] && cursor[2] != ']') {
                        for (int c = static_cast<unsigned char>(cursor[0]); c <= static_cast<unsigned char>(cursor[2]); ++c)
                            members[c] = true;
                        cursor += 3;
                    } else {
                        members[static_cast<unsigned char>(*cursor++)] = true;
                    }
                }
                for (int c = 0; c < 256; ++c)
                    set[c] = negate ? !members[c] : members[c];
                if (*cursor == '\0')
                    --cursor;
            }
            char* destination = suppress ? nullptr : va_arg(arguments, char*);
            size_t used = 0;
            int character = source.get();
            while (character != EOF && used < width && (scanset ? set[character] : !isspace(character))) {
                if (destination)
                    destination[used] = static_cast<char>(character);
                ++used;
                character = source.get();
            }
            source.unget(character);
            if (used == 0)
                return any_input || assigned ? assigned : (character == EOF ? EOF : assigned);
            any_input = true;
            if (destination) {
                destination[used] = '\0';
                ++assigned;
            }
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
}

}

extern "C" {

int vsscanf(char const* string, char const* format, va_list arguments)
{
    Source source;
    source.string = string;
    va_list copy;
    va_copy(copy, arguments);
    int result = scan(source, format, copy);
    va_end(copy);
    return result;
}

int vfscanf(FILE* file, char const* format, va_list arguments)
{
    Source source;
    source.file = file;
    va_list copy;
    va_copy(copy, arguments);
    flockfile(file);
    int result = scan(source, format, copy);
    funlockfile(file);
    va_end(copy);
    return result;
}

int vscanf(char const* format, va_list arguments)
{
    return vfscanf(stdin, format, arguments);
}

int sscanf(char const* string, char const* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vsscanf(string, format, arguments);
    va_end(arguments);
    return result;
}

int fscanf(FILE* file, char const* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vfscanf(file, format, arguments);
    va_end(arguments);
    return result;
}

int scanf(char const* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vfscanf(stdin, format, arguments);
    va_end(arguments);
    return result;
}
}
