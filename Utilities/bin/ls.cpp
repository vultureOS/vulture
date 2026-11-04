/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <locale.h>
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ls [-1aAdFGhilRrStu] [file...], as on macOS: columns on a terminal, one
// per line otherwise; -l the long listing.

namespace {

struct Options {
    bool all = false;
    bool almost_all = false;
    bool long_format = false;
    bool one_per_line = false;
    bool classify = false;
    bool human = false;
    bool recursive = false;
    bool reverse = false;
    bool by_time = false;
    bool by_size = false;
    bool directory_itself = false;
    bool inode = false;
    bool color = false;
    bool unsorted = false;
};

Options s_options;
int s_status = 0;
time_t s_now;

struct Entry {
    char* name;
    char* path;
    struct stat information;
    bool valid;
};

char const* file_type_indicator(mode_t mode)
{
    if (S_ISDIR(mode))
        return "/";
    if (S_ISLNK(mode))
        return "@";
    if (S_ISFIFO(mode))
        return "|";
    if (S_ISSOCK(mode))
        return "=";
    if (mode & 0111)
        return "*";
    return "";
}

// macOS's default LSCOLORS: blue directories, red executables, magenta
// links, yellow on black for devices.
char const* color_for(mode_t mode)
{
    if (S_ISDIR(mode))
        return mode & 01000 ? "\x1b[30;42m" : "\x1b[34m";
    if (S_ISLNK(mode))
        return "\x1b[35m";
    if (S_ISCHR(mode) || S_ISBLK(mode))
        return "\x1b[34;43m";
    if (S_ISFIFO(mode))
        return "\x1b[33m";
    if (mode & 04000)
        return "\x1b[30;41m";
    if (mode & 0111)
        return "\x1b[31m";
    return nullptr;
}

void print_name(Entry const& entry)
{
    char const* color = s_options.color && entry.valid ? color_for(entry.information.st_mode) : nullptr;
    if (color)
        fputs(color, stdout);
    fputs(entry.name, stdout);
    if (color)
        fputs("\x1b[0m", stdout);
    if (s_options.classify && entry.valid)
        fputs(file_type_indicator(entry.information.st_mode), stdout);
}

size_t name_width(Entry const& entry)
{
    size_t width = strlen(entry.name);
    if (s_options.classify && entry.valid && *file_type_indicator(entry.information.st_mode))
        ++width;
    return width;
}

void mode_string(mode_t mode, char* text)
{
    text[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : S_ISCHR(mode) ? 'c' : S_ISBLK(mode) ? 'b' : S_ISFIFO(mode) ? 'p' : S_ISSOCK(mode) ? 's' : '-';
    char const* letters = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        text[1 + i] = (mode & (0400 >> i)) ? letters[i] : '-';
    if (mode & 04000)
        text[3] = (mode & 0100) ? 's' : 'S';
    if (mode & 02000)
        text[6] = (mode & 010) ? 's' : 'S';
    if (mode & 01000)
        text[9] = (mode & 01) ? 't' : 'T';
    text[10] = '\0';
}

void human_size(long long size, char* text, size_t length)
{
    char const units[] = "BKMGTP";
    double value = static_cast<double>(size);
    int unit = 0;
    while (value >= 1024 && unit < 5) {
        value /= 1024;
        ++unit;
    }
    if (unit == 0)
        snprintf(text, length, "%lldB", size);
    else if (value < 10)
        snprintf(text, length, "%.1f%c", value, units[unit]);
    else
        snprintf(text, length, "%.0f%c", value, units[unit]);
}

int compare(Entry const& a, Entry const& b)
{
    int order = 0;
    if (s_options.by_time) {
        long long difference = static_cast<long long>(b.information.st_mtim.tv_sec - a.information.st_mtim.tv_sec);
        if (difference == 0)
            difference = b.information.st_mtim.tv_nsec - a.information.st_mtim.tv_nsec;
        order = difference < 0 ? -1 : difference > 0 ? 1 : 0;
    } else if (s_options.by_size) {
        order = b.information.st_size < a.information.st_size ? -1 : b.information.st_size > a.information.st_size ? 1 : 0;
    }
    if (order == 0)
        order = strcmp(a.name, b.name);
    return s_options.reverse ? -order : order;
}

void sort_entries(Entry* entries, size_t count)
{
    if (s_options.unsorted)
        return;
    qsort(entries, count, sizeof(Entry), [](void const* a, void const* b) {
        return compare(*static_cast<Entry const*>(a), *static_cast<Entry const*>(b));
    });
}

void print_long(Entry* entries, size_t count, bool show_total)
{
    // Column widths first.
    size_t link_width = 1, user_width = 1, group_width = 1, size_width = 1, inode_width = 1;
    long long blocks = 0;
    char buffer[64];
    for (size_t i = 0; i < count; ++i) {
        if (!entries[i].valid)
            continue;
        auto const& information = entries[i].information;
        blocks += information.st_blocks;
        link_width = max<size_t>(link_width, static_cast<size_t>(snprintf(buffer, sizeof(buffer), "%u", information.st_nlink)));
        struct passwd* user = getpwuid(information.st_uid);
        user_width = max<size_t>(user_width, user ? strlen(user->pw_name) : static_cast<size_t>(snprintf(buffer, sizeof(buffer), "%u", information.st_uid)));
        struct group* group = getgrgid(information.st_gid);
        group_width = max<size_t>(group_width, group ? strlen(group->gr_name) : static_cast<size_t>(snprintf(buffer, sizeof(buffer), "%u", information.st_gid)));
        if (S_ISCHR(information.st_mode) || S_ISBLK(information.st_mode))
            snprintf(buffer, sizeof(buffer), "%u, %u", static_cast<unsigned>(information.st_rdev >> 8), static_cast<unsigned>(information.st_rdev & 0xff));
        else if (s_options.human)
            human_size(information.st_size, buffer, sizeof(buffer));
        else
            snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(information.st_size));
        size_width = max(size_width, strlen(buffer));
        inode_width = max<size_t>(inode_width, static_cast<size_t>(snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(information.st_ino))));
    }
    if (show_total)
        printf("total %lld\n", blocks);
    for (size_t i = 0; i < count; ++i) {
        auto& entry = entries[i];
        if (!entry.valid)
            continue;
        auto const& information = entry.information;
        if (s_options.inode)
            printf("%*llu ", static_cast<int>(inode_width), static_cast<unsigned long long>(information.st_ino));
        char mode[12];
        mode_string(information.st_mode, mode);
        struct passwd* user = getpwuid(information.st_uid);
        char user_text[32], group_text[32];
        if (user)
            snprintf(user_text, sizeof(user_text), "%s", user->pw_name);
        else
            snprintf(user_text, sizeof(user_text), "%u", information.st_uid);
        struct group* group = getgrgid(information.st_gid);
        if (group)
            snprintf(group_text, sizeof(group_text), "%s", group->gr_name);
        else
            snprintf(group_text, sizeof(group_text), "%u", information.st_gid);
        char size_text[32];
        if (S_ISCHR(information.st_mode) || S_ISBLK(information.st_mode))
            snprintf(size_text, sizeof(size_text), "%u, %u", static_cast<unsigned>(information.st_rdev >> 8), static_cast<unsigned>(information.st_rdev & 0xff));
        else if (s_options.human)
            human_size(information.st_size, size_text, sizeof(size_text));
        else
            snprintf(size_text, sizeof(size_text), "%lld", static_cast<long long>(information.st_size));
        // Within six months: month day time; older: month day year.
        time_t when = information.st_mtim.tv_sec;
        struct tm broken;
        localtime_r(&when, &broken);
        char date[32];
        bool recent = when <= s_now + 3600 && s_now - when < 182 * 86400;
        strftime(date, sizeof(date), recent ? "%b %e %H:%M" : "%b %e  %Y", &broken);
        printf("%s  %*u %-*s  %-*s  %*s %s ", mode, static_cast<int>(link_width), information.st_nlink, static_cast<int>(user_width), user_text,
            static_cast<int>(group_width), group_text, static_cast<int>(size_width), size_text, date);
        print_name(entry);
        if (S_ISLNK(information.st_mode)) {
            char target[PATH_MAX];
            ssize_t length = readlink(entry.path, target, sizeof(target) - 1);
            if (length >= 0) {
                target[length] = '\0';
                printf(" -> %s", target);
            }
        }
        putchar('\n');
    }
}

void print_columns(Entry* entries, size_t count)
{
    bool terminal = isatty(STDOUT_FILENO);
    if (s_options.one_per_line || !terminal) {
        for (size_t i = 0; i < count; ++i) {
            if (s_options.inode)
                printf("%llu ", static_cast<unsigned long long>(entries[i].information.st_ino));
            print_name(entries[i]);
            putchar('\n');
        }
        return;
    }
    struct winsize size { 25, 80, 0, 0 };
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    size_t width = 1;
    for (size_t i = 0; i < count; ++i)
        width = max(width, name_width(entries[i]));
    width += 1;
    // macOS rounds the column to the next tab stop.
    width = (width + 7) & ~7ul;
    size_t columns = max<size_t>(1, size.ws_col / width);
    size_t rows = (count + columns - 1) / columns;
    for (size_t row = 0; row < rows; ++row) {
        for (size_t column = 0; column < columns; ++column) {
            size_t index = column * rows + row;
            if (index >= count)
                break;
            print_name(entries[index]);
            if ((column + 1) * rows + row < count) {
                for (size_t pad = name_width(entries[index]); pad < width; ++pad)
                    putchar(' ');
            }
        }
        putchar('\n');
    }
}

void print_entries(Entry* entries, size_t count, bool show_total)
{
    sort_entries(entries, count);
    if (s_options.long_format)
        print_long(entries, count, show_total);
    else
        print_columns(entries, count);
}

char* join(char const* directory, char const* name)
{
    size_t length = strlen(directory) + strlen(name) + 2;
    auto* path = static_cast<char*>(malloc(length));
    if (strcmp(directory, "/") == 0)
        snprintf(path, length, "/%s", name);
    else
        snprintf(path, length, "%s/%s", directory, name);
    return path;
}

void list_directory(char const* path, bool print_header)
{
    DIR* directory = opendir(path);
    if (!directory) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        s_status = 1;
        return;
    }
    size_t capacity = 64, count = 0;
    auto* entries = static_cast<Entry*>(malloc(capacity * sizeof(Entry)));
    while (struct dirent* item = readdir(directory)) {
        bool dot = item->d_name[0] == '.';
        bool dot_or_dotdot = strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0;
        if (dot && !s_options.all && !(s_options.almost_all && !dot_or_dotdot))
            continue;
        if (count == capacity) {
            capacity *= 2;
            entries = static_cast<Entry*>(realloc(entries, capacity * sizeof(Entry)));
        }
        auto& entry = entries[count++];
        entry.name = strdup(item->d_name);
        entry.path = join(path, item->d_name);
        entry.valid = lstat(entry.path, &entry.information) == 0;
        if (!entry.valid)
            memset(&entry.information, 0, sizeof(entry.information));
    }
    closedir(directory);
    if (print_header)
        printf("%s:\n", path);
    print_entries(entries, count, true);
    if (s_options.recursive) {
        for (size_t i = 0; i < count; ++i) {
            if (!entries[i].valid || !S_ISDIR(entries[i].information.st_mode))
                continue;
            if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0)
                continue;
            putchar('\n');
            list_directory(entries[i].path, true);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        free(entries[i].name);
        free(entries[i].path);
    }
    free(entries);
}

}

int main(int argc, char** argv)
{
    setlocale(LC_ALL, "");
    s_now = time(nullptr);
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "--") == 0) {
            ++first;
            break;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            switch (*option) {
            case 'a':
                s_options.all = true;
                break;
            case 'A':
                s_options.almost_all = true;
                break;
            case 'l':
                s_options.long_format = true;
                break;
            case '1':
                s_options.one_per_line = true;
                break;
            case 'F':
                s_options.classify = true;
                break;
            case 'h':
                s_options.human = true;
                break;
            case 'R':
                s_options.recursive = true;
                break;
            case 'r':
                s_options.reverse = true;
                break;
            case 't':
                s_options.by_time = true;
                break;
            case 'S':
                s_options.by_size = true;
                break;
            case 'd':
                s_options.directory_itself = true;
                break;
            case 'i':
                s_options.inode = true;
                break;
            case 'G':
                s_options.color = true;
                break;
            case 'f':
                s_options.unsorted = s_options.all = true;
                break;
            default:
                fprintf(stderr, "ls: invalid option -- %c\nusage: ls [-1AFGRSadfhilrt] [file ...]\n", *option);
                return 1;
            }
        }
    }
    if (char const* clicolor = getenv("CLICOLOR"); clicolor && isatty(STDOUT_FILENO))
        s_options.color = true;

    char const* default_argument[] = { "." };
    char const** arguments = first < argc ? const_cast<char const**>(argv + first) : default_argument;
    size_t argument_count = first < argc ? static_cast<size_t>(argc - first) : 1;

    // Files first, then each directory under a header (when there are
    // several arguments).
    auto* files = static_cast<Entry*>(calloc(argument_count, sizeof(Entry)));
    auto* directories = static_cast<Entry*>(calloc(argument_count, sizeof(Entry)));
    size_t file_count = 0, directory_count = 0;
    for (size_t i = 0; i < argument_count; ++i) {
        Entry entry {};
        entry.name = const_cast<char*>(arguments[i]);
        entry.path = const_cast<char*>(arguments[i]);
        // Follow a link named on the command line unless -l or -d.
        int result = (s_options.long_format || s_options.directory_itself) ? lstat(arguments[i], &entry.information) : stat(arguments[i], &entry.information);
        if (result < 0) {
            fprintf(stderr, "ls: %s: %s\n", arguments[i], strerror(errno));
            s_status = 1;
            continue;
        }
        entry.valid = true;
        if (S_ISDIR(entry.information.st_mode) && !s_options.directory_itself)
            directories[directory_count++] = entry;
        else
            files[file_count++] = entry;
    }
    if (file_count > 0)
        print_entries(files, file_count, false);
    sort_entries(directories, directory_count);
    for (size_t i = 0; i < directory_count; ++i) {
        if (file_count > 0 || i > 0)
            putchar('\n');
        list_directory(directories[i].path, argument_count > 1 || s_options.recursive);
    }
    fflush(stdout);
    return s_status;
}
