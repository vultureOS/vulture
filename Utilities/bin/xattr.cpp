/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/xattr.h>

// xattr: what's attached to a file, as on a Mac.

namespace {

enum class Action {
    List,
    Print,
    Write,
    Delete,
    Clear,
};

Action s_action = Action::List;
bool s_long = false;
bool s_recursive = false;
bool s_link_itself = false;
bool s_verbose = false;
bool s_hex = false;
bool s_name_files = false;
int s_status = 0;

[[noreturn]] void usage(int status)
{
    fprintf(stderr, "usage: xattr [-l] [-r] [-s] [-v] [-x] file [file ...]\n"
                    "       xattr -p [-l] [-r] [-s] [-v] [-x] attr_name file [file ...]\n"
                    "       xattr -w [-r] [-s] [-x] attr_name attr_value file [file ...]\n"
                    "       xattr -d [-r] [-s] attr_name file [file ...]\n"
                    "       xattr -c [-r] [-s] file [file ...]\n"
                    "\n"
                    "The first form lists the names of all xattrs on the given file(s).\n"
                    "The second form (-p) prints the value of the xattr attr_name.\n"
                    "The third form (-w) sets the value of the xattr attr_name to the string attr_value.\n"
                    "The fourth form (-d) deletes the xattr attr_name.\n"
                    "The fifth form (-c) deletes (clears) all xattrs.\n"
                    "\n"
                    "options:\n"
                    "  -h: print this help\n"
                    "  -l: print long format (attr_name: attr_value and hex output has offsets and\n"
                    "      ascii representation)\n"
                    "  -r: act recursively\n"
                    "  -s: act on the symbolic link itself rather than what the link points to\n"
                    "  -v: also print filename (automatic with -r and with multiple files)\n"
                    "  -x: attr_value is represented as a hex string for input and output\n");
    exit(status);
}

int link_option()
{
    return s_link_itself ? XATTR_NOFOLLOW : 0;
}

void print_hex_dump(unsigned char const* bytes, size_t size)
{
    for (size_t offset = 0; offset < size; offset += 16) {
        printf("%08zX ", offset);
        for (size_t i = 0; i < 16; ++i) {
            if (offset + i < size)
                printf(" %02X", bytes[offset + i]);
            else
                printf("   ");
        }
        printf("  |");
        for (size_t i = 0; i < 16 && offset + i < size; ++i) {
            unsigned char const c = bytes[offset + i];
            putchar(c >= 0x20 && c < 0x7f ? c : '.');
        }
        printf("|\n");
    }
    printf("%08zX\n", size);
}

void print_hex_flat(unsigned char const* bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        printf("%02X%s", bytes[i], (i + 1) % 16 == 0 && i + 1 < size ? "\n" : " ");
    putchar('\n');
}

// A value that's text, up to its first zero, is said as text.
void print_as_text(unsigned char const* bytes, size_t size)
{
    size_t length = 0;
    while (length < size && bytes[length])
        ++length;
    fwrite(bytes, 1, length, stdout);
}

void say_file(char const* path)
{
    if (s_name_files)
        printf("%s: ", path);
}

void show_value(char const* path, char const* name, bool with_file, bool with_name)
{
    ssize_t const size = getxattr(path, name, nullptr, 0, 0, link_option());
    if (size < 0) {
        fprintf(stderr, "xattr: %s: No such xattr: %s\n", path, name);
        s_status = 1;
        return;
    }
    auto* value = static_cast<unsigned char*>(malloc(static_cast<size_t>(size) + 1));
    ssize_t const got = getxattr(path, name, value, static_cast<size_t>(size), 0, link_option());
    if (got >= 0) {
        if (with_file)
            say_file(path);
        if (s_long) {
            if (with_name)
                printf("%s:", name);
            if (s_hex) {
                putchar('\n');
                print_hex_dump(value, static_cast<size_t>(got));
            } else {
                printf(" ");
                print_as_text(value, static_cast<size_t>(got));
                putchar('\n');
            }
        } else if (s_hex) {
            print_hex_flat(value, static_cast<size_t>(got));
        } else {
            print_as_text(value, static_cast<size_t>(got));
            putchar('\n');
        }
    }
    free(value);
}

// In the order a Mac gives them: sorted.
char** names_of(char const* path, size_t& count, char*& storage)
{
    count = 0;
    storage = nullptr;
    ssize_t const size = listxattr(path, nullptr, 0, link_option());
    if (size <= 0)
        return nullptr;
    storage = static_cast<char*>(malloc(static_cast<size_t>(size)));
    ssize_t const got = listxattr(path, storage, static_cast<size_t>(size), link_option());
    if (got <= 0)
        return nullptr;
    for (ssize_t at = 0; at < got; at += static_cast<ssize_t>(strlen(storage + at)) + 1)
        ++count;
    auto** names = static_cast<char**>(malloc(sizeof(char*) * count));
    size_t next = 0;
    for (ssize_t at = 0; at < got; at += static_cast<ssize_t>(strlen(storage + at)) + 1)
        names[next++] = storage + at;
    for (size_t i = 1; i < count; ++i) {
        for (size_t j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; --j) {
            auto* swapped = names[j];
            names[j] = names[j - 1];
            names[j - 1] = swapped;
        }
    }
    return names;
}

unsigned char* parse_hex(char const* text, size_t& size)
{
    auto* bytes = static_cast<unsigned char*>(malloc(strlen(text) / 2 + 1));
    size = 0;
    int high = -1;
    for (char const* c = text; *c; ++c) {
        int digit;
        if (*c >= '0' && *c <= '9')
            digit = *c - '0';
        else if (*c >= 'a' && *c <= 'f')
            digit = *c - 'a' + 10;
        else if (*c >= 'A' && *c <= 'F')
            digit = *c - 'A' + 10;
        else if (*c == ' ' || *c == '\t' || *c == '\n')
            continue;
        else
            return nullptr;
        if (high < 0) {
            high = digit;
        } else {
            bytes[size++] = static_cast<unsigned char>(high << 4 | digit);
            high = -1;
        }
    }
    return high < 0 ? bytes : nullptr;
}

char const* s_name = nullptr;
char const* s_value = nullptr;

void act_on(char const* path)
{
    struct stat information;
    if ((s_link_itself ? lstat(path, &information) : stat(path, &information)) != 0) {
        fprintf(stderr, "xattr: No such file: %s\n", path);
        s_status = 1;
        return;
    }
    switch (s_action) {
    case Action::Print:
        show_value(path, s_name, s_verbose || s_name_files, s_long);
        break;
    case Action::Write: {
        int result;
        if (s_hex) {
            size_t size = 0;
            auto* bytes = parse_hex(s_value, size);
            if (!bytes) {
                fprintf(stderr, "xattr: bad hex encoding: %s\n", s_value);
                s_status = 1;
                return;
            }
            result = setxattr(path, s_name, bytes, size, 0, link_option());
            free(bytes);
        } else {
            result = setxattr(path, s_name, s_value, strlen(s_value), 0, link_option());
        }
        if (result != 0) {
            fprintf(stderr, "xattr: [Errno %d] %s: '%s'\n", errno, strerror(errno), path);
            s_status = 1;
        }
        break;
    }
    case Action::Delete:
        if (removexattr(path, s_name, link_option()) != 0) {
            fprintf(stderr, "xattr: %s: No such xattr: %s\n", path, s_name);
            s_status = 1;
        }
        break;
    case Action::Clear:
    case Action::List: {
        size_t count = 0;
        char* storage = nullptr;
        char** names = names_of(path, count, storage);
        for (size_t i = 0; i < count; ++i) {
            if (s_action == Action::Clear) {
                if (removexattr(path, names[i], link_option()) != 0)
                    s_status = 1;
            } else if (s_long) {
                show_value(path, names[i], true, true);
            } else {
                say_file(path);
                printf("%s\n", names[i]);
            }
        }
        free(names);
        free(storage);
        break;
    }
    }
}

void walk(char const* path)
{
    // Down first, in listing order: what's inside comes before what it's in, as a Mac's does.
    struct stat information;
    if (s_recursive && lstat(path, &information) == 0 && S_ISDIR(information.st_mode)) {
        if (DIR* directory = opendir(path)) {
            while (auto* entry = readdir(directory)) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;
                char inner[PATH_MAX];
                snprintf(inner, sizeof(inner), "%s/%s", path, entry->d_name);
                walk(inner);
            }
            closedir(directory);
        }
    }
    act_on(path);
}

}

int main(int argc, char** argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "--") == 0) {
            ++first;
            break;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            switch (*option) {
            case 'c':
                s_action = Action::Clear;
                break;
            case 'd':
                s_action = Action::Delete;
                break;
            case 'h':
                usage(0);
            case 'l':
                s_long = true;
                break;
            case 'p':
                s_action = Action::Print;
                break;
            case 'r':
                s_recursive = true;
                break;
            case 's':
                s_link_itself = true;
                break;
            case 'v':
                s_verbose = true;
                break;
            case 'w':
                s_action = Action::Write;
                break;
            case 'x':
                s_hex = true;
                break;
            default:
                fprintf(stderr, "xattr: option -%c not recognized\n\n", *option);
                usage(1);
            }
        }
    }
    if (s_action == Action::Print || s_action == Action::Delete || s_action == Action::Write) {
        if (first >= argc)
            usage(1);
        s_name = argv[first++];
    }
    if (s_action == Action::Write) {
        if (first >= argc)
            usage(1);
        s_value = argv[first++];
    }
    if (first >= argc)
        usage(1);
    s_name_files = s_verbose || s_recursive || argc - first > 1;
    for (int i = first; i < argc; ++i)
        walk(argv[i]);
    return s_status;
}
