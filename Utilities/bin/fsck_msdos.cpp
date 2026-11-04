/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include "ForeignStore.h"
#include <time.h>

// fsck_msdos and fsck_exfat: -n only looks, which is also what happens with neither -y nor -p; those two put right what they can.

int main(int argc, char** argv)
{
    bool const exfat = strcmp(last_component(argv[0]), "fsck_exfat") == 0;
    bool repair = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'y' || *option == 'p')
                repair = true;
            else if (*option == 'n' || *option == 'q')
                repair = false;
        }
    }
    if (first >= argc) {
        fprintf(stderr, exfat ? "Usage: fsck_exfat [-d] [-f] [-p | -q | -y | -n] [-S directory] [-g | -x] device\n" : "Usage: fsck_msdos [-fnpqy] filesystem ...\n");
        return 8;
    }
    auto store = ForeignStore::open_path(argv[first], !repair);
    if (store.is_error()) {
        fprintf(stderr, "%s: %s: %s\n", last_component(argv[0]), argv[first], strerror(store.error().code()));
        return 8;
    }
    auto const clock = []() -> i64 { return static_cast<i64>(time(nullptr)); };
    auto const look = [&] { return exfat ? ForeignFS::check_exfat(*store.value(), clock) : ForeignFS::check_fat(*store.value(), clock); };
    auto report = look();
    if (report.is_error()) {
        fprintf(stderr, "%s: %s: %s\n", last_component(argv[0]), argv[first], strerror(report.error().code()));
        return 8;
    }
    printf("** %s\n", argv[first]);
    if (!report.value().is_clean() && repair) {
        auto volume = ForeignFS::open_volume(*store.value(), clock, false);
        if (volume.is_error()) {
            fprintf(stderr, "%s: %s: %s\n", last_component(argv[0]), argv[first], strerror(volume.error().code()));
            return 8;
        }
        auto const repaired = volume.value()->repair([](StringView text) {
            printf("%.*s\n", static_cast<int>(text.length()), text.characters_without_null_termination());
        });
        if (repaired.is_error()) {
            fprintf(stderr, "%s: can't repair %s: %s\n", last_component(argv[0]), argv[first], strerror(repaired.error().code()));
            return 8;
        }
        report = look();
        if (report.is_error())
            return 8;
    }
    for (auto const& problem : report.value().problems)
        printf("%s\n", problem.characters());
    printf("%llu files, %llu directories\n", static_cast<unsigned long long>(report.value().files), static_cast<unsigned long long>(report.value().directories));
    if (report.value().is_clean()) {
        printf("** The volume is OK.\n");
        return 0;
    }
    printf("** The volume was found corrupt and needs to be repaired.\n");
    return 8;
}
