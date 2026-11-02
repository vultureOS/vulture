/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibPackage/Manager.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// pkg update | search [word] | install name | remove name | list | info name

namespace {

StringView view_of(char const* text)
{
    return { text, strlen(text) };
}

int report(Error const& error)
{
    if (error.is_errno())
        fprintf(stderr, "pkg: %s\n", strerror(error.code()));
    else
        fprintf(stderr, "pkg: %.*s\n", static_cast<int>(error.string_literal().length()), error.string_literal().characters_without_null_termination());
    return 1;
}

bool needs_root()
{
    if (geteuid() == 0)
        return false;
    fprintf(stderr, "pkg: that takes root: try sudo\n");
    return true;
}

void usage()
{
    fprintf(stderr, "usage: pkg update\n       pkg search [word]\n       pkg install name\n       pkg remove name\n       pkg list\n       pkg info name\n");
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    StringView const command = view_of(argv[1]);
    if (command == "update"_sv) {
        if (needs_root())
            return 1;
        auto result = Package::refresh();
        if (result.is_error())
            return report(result.error());
        auto all = Package::available();
        if (all.is_error())
            return report(all.error());
        printf("pkg: %zu package%s available\n", all.value().size(), all.value().size() == 1 ? "" : "s");
        return 0;
    }
    if (command == "search"_sv) {
        StringView const word = argc > 2 ? view_of(argv[2]) : StringView {};
        auto all = Package::available();
        if (all.is_error())
            return report(all.error());
        for (auto const& package : all.value()) {
            if (word.is_empty() || package.name.view().contains(word) || package.summary.view().contains(word))
                printf("%-24s %-10s %s\n", package.name.characters(), package.version.characters(), package.summary.characters());
        }
        return 0;
    }
    if (command == "install"_sv && argc == 3) {
        if (needs_root())
            return 1;
        u32 last = 0;
        bool const terminal = isatty(1);
        auto result = Package::install(view_of(argv[2]), [&](u32 percent) {
            if (terminal && percent != last) {
                printf("\r%3u%%", percent);
                fflush(stdout);
            }
            last = percent;
        });
        if (terminal)
            printf("\r    \r");
        if (result.is_error())
            return report(result.error());
        printf("pkg: installed %s\n", argv[2]);
        return 0;
    }
    if (command == "remove"_sv && argc == 3) {
        if (needs_root())
            return 1;
        auto result = Package::remove(view_of(argv[2]));
        if (result.is_error())
            return report(result.error());
        printf("pkg: removed %s\n", argv[2]);
        return 0;
    }
    if (command == "list"_sv) {
        auto all = Package::installed();
        if (all.is_error())
            return report(all.error());
        for (auto const& package : all.value())
            printf("%-24s %-10s %s\n", package.name.characters(), package.version.characters(), package.summary.characters());
        return 0;
    }
    if (command == "info"_sv && argc == 3) {
        auto all = Package::installed();
        if (all.is_error())
            return report(all.error());
        for (auto const& package : all.value()) {
            if (package.name.view() != view_of(argv[2]))
                continue;
            printf("name: %s\nversion: %s\nsummary: %s\nfiles:\n", package.name.characters(), package.version.characters(), package.summary.characters());
            for (auto const& file : package.files)
                printf("  /%s\n", file.characters());
            return 0;
        }
        fprintf(stderr, "pkg: %s isn't installed\n", argv[2]);
        return 1;
    }
    usage();
    return 2;
}
