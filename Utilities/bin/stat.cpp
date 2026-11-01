/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <time.h>

// stat [-L] [-f format] file...: everything stat() says, or a format with
// %N name, %z size, %p mode (octal), %Sp mode (rwx), %u/%Su owner,
// %g/%Sg group, %i inode, %l links, %m/%Sm modified, %HT file type.
namespace {

char const* type_name(mode_t mode)
{
    if (S_ISREG(mode))
        return "Regular File";
    if (S_ISDIR(mode))
        return "Directory";
    if (S_ISLNK(mode))
        return "Symbolic Link";
    if (S_ISCHR(mode))
        return "Character Device";
    if (S_ISBLK(mode))
        return "Block Device";
    if (S_ISFIFO(mode))
        return "FIFO";
    return "Unknown";
}

void mode_text(mode_t mode, char* text)
{
    text[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : S_ISCHR(mode) ? 'c' : S_ISBLK(mode) ? 'b' : S_ISFIFO(mode) ? 'p' : '-';
    for (int i = 0; i < 9; ++i)
        text[1 + i] = (mode & (0400 >> i)) ? "rwxrwxrwx"[i] : '-';
    text[10] = '\0';
}

void date_text(struct timespec const& when, char* text, size_t size)
{
    struct tm broken;
    time_t seconds = when.tv_sec;
    localtime_r(&seconds, &broken);
    strftime(text, size, "%b %e %H:%M:%S %Y", &broken);
}

}

int main(int argc, char** argv)
{
    bool follow = false;
    char const* format = nullptr;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        if (strcmp(argv[first], "-L") == 0)
            follow = true;
        else if (strcmp(argv[first], "-f") == 0 && first + 1 < argc)
            format = argv[++first];
    }
    int status = 0;
    for (int i = first; i < argc; ++i) {
        struct stat information;
        if ((follow ? stat(argv[i], &information) : lstat(argv[i], &information)) < 0) {
            warn("stat", argv[i]);
            status = 1;
            continue;
        }
        struct passwd* user = getpwuid(information.st_uid);
        struct group* group = getgrgid(information.st_gid);
        char mode[12], modified[64], accessed[64], changed[64], born[64];
        mode_text(information.st_mode, mode);
        date_text(information.st_mtim, modified, sizeof(modified));
        date_text(information.st_atim, accessed, sizeof(accessed));
        date_text(information.st_ctim, changed, sizeof(changed));
        date_text(information.st_birthtimespec, born, sizeof(born));
        if (!format) {
            printf("  File: %s\n  Size: %lld\t\tBlocks: %lld\tIO Block: %lld\t%s\n", argv[i], static_cast<long long>(information.st_size),
                static_cast<long long>(information.st_blocks), static_cast<long long>(information.st_blksize), type_name(information.st_mode));
            printf("Device: %llu\tInode: %llu\tLinks: %u\n", static_cast<unsigned long long>(information.st_dev),
                static_cast<unsigned long long>(information.st_ino), information.st_nlink);
            printf("Access: (%04o/%s)  Uid: (%5u/%8s)   Gid: (%5u/%8s)\n", information.st_mode & 07777, mode, information.st_uid, user ? user->pw_name : "?",
                information.st_gid, group ? group->gr_name : "?");
            printf("Access: %s\nModify: %s\nChange: %s\n Birth: %s\n", accessed, modified, changed, born);
            continue;
        }
        for (char const* cursor = format; *cursor; ++cursor) {
            if (*cursor == '\\' && cursor[1] == 'n') {
                putchar('\n');
                ++cursor;
                continue;
            }
            if (*cursor != '%' || !cursor[1]) {
                putchar(*cursor);
                continue;
            }
            bool string_form = cursor[1] == 'S';
            if (string_form)
                ++cursor;
            char letter = *++cursor;
            switch (letter) {
            case 'N':
                fputs(argv[i], stdout);
                break;
            case 'z':
                printf("%lld", static_cast<long long>(information.st_size));
                break;
            case 'p':
                string_form ? fputs(mode, stdout) : printf("%o", information.st_mode);
                break;
            case 'u':
                string_form && user ? fputs(user->pw_name, stdout) : printf("%u", information.st_uid);
                break;
            case 'g':
                string_form && group ? fputs(group->gr_name, stdout) : printf("%u", information.st_gid);
                break;
            case 'i':
                printf("%llu", static_cast<unsigned long long>(information.st_ino));
                break;
            case 'l':
                printf("%u", information.st_nlink);
                break;
            case 'm':
                string_form ? fputs(modified, stdout) : printf("%lld", static_cast<long long>(information.st_mtime));
                break;
            case 'a':
                string_form ? fputs(accessed, stdout) : printf("%lld", static_cast<long long>(information.st_atime));
                break;
            case 'H':
                if (cursor[1] == 'T') {
                    ++cursor;
                    fputs(type_name(information.st_mode), stdout);
                }
                break;
            case '%':
                putchar('%');
                break;
            default:
                putchar('%');
                putchar(letter);
            }
        }
        putchar('\n');
    }
    return status;
}
