/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// od [-bcdox] [-A base] [-N count] [-j skip] [file...]: bytes, as something to look at. Sixteen to a line, with where they are.

namespace {

enum class Format {
    Octal2,  // The default: 16-bit words, in octal.
    Octal1,  // -b
    Char,    // -c
    Decimal2, // -d
    Hex2,    // -x
};

Format g_format = Format::Octal2;
char g_address_base = 'o';
long long g_limit = -1;
long long g_skip = 0;

void print_address(long long address)
{
    switch (g_address_base) {
    case 'n':
        break;
    case 'd':
        printf("%07lld", address);
        break;
    case 'x':
        printf("%07llx", address);
        break;
    default:
        printf("%07llo", address);
        break;
    }
}

void print_character(unsigned char c)
{
    switch (c) {
    case 0:
        printf("  \\0");
        break;
    case '\a':
        printf("  \\a");
        break;
    case '\b':
        printf("  \\b");
        break;
    case '\f':
        printf("  \\f");
        break;
    case '\n':
        printf("  \\n");
        break;
    case '\r':
        printf("  \\r");
        break;
    case '\t':
        printf("  \\t");
        break;
    case '\v':
        printf("  \\v");
        break;
    default:
        if (c >= 32 && c < 127)
            printf("   %c", c);
        else
            printf(" %03o", c);
        break;
    }
}

void print_line(long long address, unsigned char const* bytes, size_t count)
{
    print_address(address);
    switch (g_format) {
    case Format::Char:
        putchar(' ');
        for (size_t i = 0; i < count; ++i)
            print_character(bytes[i]);
        break;
    case Format::Octal1:
        for (size_t i = 0; i < count; ++i)
            printf(" %03o", bytes[i]);
        break;
    default:
        for (size_t i = 0; i < count; i += 2) {
            unsigned const word = bytes[i] | (i + 1 < count ? static_cast<unsigned>(bytes[i + 1]) << 8 : 0);
            if (g_format == Format::Octal2)
                printf(" %06o", word);
            else if (g_format == Format::Decimal2)
                printf(" %5u", word);
            else
                printf(" %04x", word);
        }
        break;
    }
    // In twos, as it's all laid out: half of one's still the whole width of one.
    if (g_format == Format::Char && (count & 1))
        printf("    ");
    putchar('\n');
}

}

int main(int argc, char** argv)
{
    int option;
    while ((option = getopt(argc, argv, "bcdoxA:N:j:")) != -1) {
        switch (option) {
        case 'b':
            g_format = Format::Octal1;
            break;
        case 'c':
            g_format = Format::Char;
            break;
        case 'd':
            g_format = Format::Decimal2;
            break;
        case 'o':
            g_format = Format::Octal2;
            break;
        case 'x':
            g_format = Format::Hex2;
            break;
        case 'A':
            g_address_base = optarg[0];
            break;
        case 'N':
            g_limit = atoll(optarg);
            break;
        case 'j':
            g_skip = atoll(optarg);
            break;
        default:
            fprintf(stderr, "usage: od [-bcdox] [-A base] [-N count] [-j skip] [file ...]\n");
            return 1;
        }
    }

    unsigned char line[16];
    size_t filled = 0;
    long long address = 0;
    long long skipped = 0;
    long long taken = 0;
    unsigned char previous[16];
    bool have_previous = false, printed_star = false;
    int status = 0;
    auto flush_line = [&]() {
        if (filled == 16 && have_previous && memcmp(line, previous, 16) == 0) {
            if (!printed_star)
                puts("*");
            printed_star = true;
        } else {
            print_line(address, line, filled);
            printed_star = false;
        }
        if (filled == 16) {
            memcpy(previous, line, 16);
            have_previous = true;
        }
        address += static_cast<long long>(filled);
        filled = 0;
    };

    int const file_count = argc - optind;
    for (int index = 0; index < (file_count ? file_count : 1); ++index) {
        int fd = 0;
        if (file_count) {
            fd = open(argv[optind + index], O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "od: %s: No such file or directory\n", argv[optind + index]);
                status = 1;
                continue;
            }
        }
        unsigned char buffer[4096];
        ssize_t got;
        while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
            for (ssize_t i = 0; i < got; ++i) {
                if (skipped < g_skip) {
                    ++skipped;
                    ++address;
                    continue;
                }
                if (g_limit >= 0 && taken >= g_limit)
                    break;
                ++taken;
                line[filled++] = buffer[i];
                if (filled == 16)
                    flush_line();
            }
        }
        if (file_count)
            close(fd);
    }
    if (filled)
        flush_line();
    if (g_address_base != 'n') {
        print_address(address);
        putchar('\n');
    }
    return status;
}
