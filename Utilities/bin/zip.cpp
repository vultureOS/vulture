/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TreeHelpers.h"
#include <LibArchive/Zip.h>

// zip [-qry] archive file...: -r goes down directories, -y keeps a symbolic link as what it is.

int main(int argc, char** argv)
{
    bool recursive = false, keep_links = false, quiet = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            switch (*option) {
            case 'r':
                recursive = true;
                break;
            case 'y':
                keep_links = true;
                break;
            case 'q':
                quiet = true;
                break;
            default:
                fprintf(stderr, "zip error: Invalid command arguments (no such option: %c)\n", *option);
                return 16;
            }
        }
    }
    if (argc - first < 2) {
        fprintf(stderr, "usage: zip [-qry] archive file ...\n");
        return 16;
    }
    char archive[1024];
    snprintf(archive, sizeof(archive), "%s", argv[first++]);
    if (!strchr(last_component(archive), '.'))
        strncat(archive, ".zip", sizeof(archive) - strlen(archive) - 1);

    Archive::ZipWriter writer;
    int status = 0;
    for (int i = first; i < argc; ++i) {
        char const* name = argv[i];
        struct stat information;
        if (lstat(name, &information) != 0) {
            fprintf(stderr, "\tzip warning: name not matched: %s\n", name);
            status = 12;
            continue;
        }
        auto add_one = [&](char const* relative, char const* full, struct stat const& status_of) {
            u16 date = 0, clock = 0;
            to_dos_time(status_of.st_mtime, date, clock);
            StringView member { relative };
            while (member.starts_with('/'))
                member = member.substring_view(1);
            if (S_ISDIR(status_of.st_mode)) {
                if (!quiet)
                    printf("  adding: %s/ (stored 0%%)\n", relative);
                MUST(writer.add_directory(member, status_of.st_mode, date, clock));
                return;
            }
            if (S_ISLNK(status_of.st_mode) && keep_links) {
                char target[1024];
                ssize_t const length = readlink(full, target, sizeof(target) - 1);
                target[length > 0 ? length : 0] = 0;
                if (!quiet)
                    printf("  adding: %s (stored 0%%)\n", relative);
                MUST(writer.add_symbolic_link(member, StringView(target), date, clock));
                return;
            }
            auto contents = read_whole_file(full);
            if (contents.is_error()) {
                fprintf(stderr, "\tzip warning: could not open for reading: %s\n", full);
                status = 18;
                return;
            }
            if (!quiet)
                printf("  adding: %s (deflated)\n", relative);
            MUST(writer.add_file(member, status_of.st_mode, contents.value().span(), date, clock));
        };
        if (recursive && S_ISDIR(information.st_mode)) {
            walk_tree(name, name, add_one);
        } else if (S_ISDIR(information.st_mode)) {
            add_one(name, name, information);
        } else {
            add_one(name, name, information);
        }
    }
    auto finished = writer.finish();
    if (finished.is_error() || !write_whole_file(archive, finished.value().data(), finished.value().size(), 0644)) {
        fprintf(stderr, "zip error: could not write %s\n", archive);
        return 14;
    }
    return status;
}
