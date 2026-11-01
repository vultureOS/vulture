/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <dirent.h>
#include <sys/stat.h>

// du [-a | -s | -d depth] [-hk] [-c] [file...]: disk usage in 512-byte
// blocks (or -k kilobytes, -h human units).
namespace {

bool s_all = false, s_summary = false, s_human = false, s_kilobytes = false;
int s_depth = -1;

void print(long long blocks, char const* path)
{
    if (s_human) {
        double bytes = static_cast<double>(blocks) * 512;
        char const units[] = "BKMGT";
        int unit = 0;
        while (bytes >= 1024 && unit < 4) {
            bytes /= 1024;
            ++unit;
        }
        printf(bytes < 10 && unit > 0 ? "%.1f%c\t%s\n" : "%.0f%c\t%s\n", bytes, units[unit], path);
    } else {
        printf("%lld\t%s\n", s_kilobytes ? (blocks + 1) / 2 : blocks, path);
    }
}

long long usage(char const* path, int depth)
{
    struct stat information;
    if (lstat(path, &information) < 0) {
        warn("du", path);
        return 0;
    }
    long long blocks = information.st_blocks;
    if (S_ISDIR(information.st_mode)) {
        DIR* directory = opendir(path);
        if (directory) {
            while (struct dirent* entry = readdir(directory)) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;
                char* child = path_join(path, entry->d_name);
                blocks += usage(child, depth + 1);
                free(child);
            }
            closedir(directory);
        }
        if (!s_summary && (s_depth < 0 || depth <= s_depth))
            print(blocks, path);
    } else if ((s_all && (s_depth < 0 || depth <= s_depth)) || depth == 0) {
        if (!s_summary)
            print(blocks, path);
    }
    return blocks;
}

}

int main(int argc, char** argv)
{
    bool total = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        if (strcmp(argv[first], "-d") == 0 && first + 1 < argc) {
            s_depth = atoi(argv[++first]);
            continue;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'a')
                s_all = true;
            else if (*option == 's')
                s_summary = true;
            else if (*option == 'h')
                s_human = true;
            else if (*option == 'k')
                s_kilobytes = true;
            else if (*option == 'c')
                total = true;
        }
    }
    long long sum = 0;
    for (int i = first; i < argc || i == first; ++i) {
        char const* path = i < argc ? argv[i] : ".";
        long long blocks = usage(path, 0);
        if (s_summary)
            print(blocks, path);
        sum += blocks;
        if (i >= argc)
            break;
    }
    if (total)
        print(sum, "total");
    return 0;
}
