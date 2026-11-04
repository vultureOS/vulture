/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include "ForeignStore.h"
#include <time.h>

// newfs_msdos and newfs_exfat: as a Mac's have them, for what they can be asked.

int main(int argc, char** argv)
{
    bool const exfat = strcmp(last_component(argv[0]), "newfs_exfat") == 0;
    unsigned bits = 0;
    char const* label = "";
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        if (strcmp(argv[first], "-F") == 0 && first + 1 < argc)
            bits = static_cast<unsigned>(atoi(argv[++first]));
        else if ((strcmp(argv[first], "-v") == 0) && first + 1 < argc)
            label = argv[++first];
    }
    if (first >= argc) {
        fprintf(stderr, exfat ? "usage: newfs_exfat [options...] special_device\n" : "usage: newfs_msdos [ -options ] special [disktype]\n");
        return 1;
    }
    auto store = ForeignStore::open_path(argv[first], false);
    if (store.is_error()) {
        fprintf(stderr, "%s: %s: %s\n", last_component(argv[0]), argv[first], strerror(store.error().code()));
        return 1;
    }
    auto const clock = []() -> i64 { return static_cast<i64>(time(nullptr)); };
    auto const made = exfat ? ForeignFS::format_exfat(*store.value(), { label, strlen(label) }, clock)
                            : ForeignFS::format_fat(*store.value(), { label, strlen(label) }, clock, bits);
    if (made.is_error()) {
        fprintf(stderr, "%s: %s: %s\n", last_component(argv[0]), argv[first], strerror(made.error().code()));
        return 1;
    }
    return 0;
}
