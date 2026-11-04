/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TreeHelpers.h"
#include <LibArchive/Tar.h>
#include <LibArchive/Zip.h>

// unzip [-lnoqt] [-d directory] archive [member...]
int main(int argc, char** argv)
{
    bool to_output = false, names_only = false;
    bool list = false, test = false, quiet = false, overwrite = false, never_overwrite = false;
    char const* directory = nullptr;
    char const* archive_path = nullptr;
    Vector<char const*> members;
    for (int i = 1; i < argc; ++i) {
        char const* argument = argv[i];
        if (argument[0] == '-' && argument[1] && !archive_path) {
            for (char const* option = argument + 1; *option; ++option) {
                switch (*option) {
                case 'l':
                    list = true;
                    break;
                case 't':
                    test = true;
                    break;
                case 'q':
                    quiet = true;
                    break;
                case 'o':
                    overwrite = true;
                    break;
                case 'p':
                    to_output = true;
                    break;
                case 'Z':
                    // zipinfo's -1: the names, and nothing else, is the only part of it anything uses.
                    names_only = true;
                    option = "x";
                    break;
                case 'n':
                    never_overwrite = true;
                    break;
                case 'd':
                    directory = option[1] ? option + 1 : (i + 1 < argc ? argv[++i] : nullptr);
                    option = "x";
                    break;
                default:
                    fprintf(stderr, "usage: unzip [-lnopqt] [-Z1] [-d directory] archive [member ...]\n");
                    return 10;
                }
            }
        } else if (!archive_path) {
            archive_path = argument;
        } else {
            members.append(argument);
        }
    }
    if (!archive_path) {
        fprintf(stderr, "usage: unzip [-lnopqt] [-Z1] [-d directory] archive [member ...]\n");
        return 10;
    }
    auto bytes = read_whole_file(archive_path);
    if (bytes.is_error()) {
        fprintf(stderr, "unzip:  cannot find or open %s, %s.zip or %s.ZIP.\n", archive_path, archive_path, archive_path);
        return 9;
    }
    auto reader = Archive::ZipReader::parse(bytes.value().span());
    if (reader.is_error()) {
        fprintf(stderr, "  End-of-central-directory signature not found. This file isn't a zip file: %s\n", archive_path);
        return 9;
    }
    auto wanted = [&](StringView path) {
        if (members.is_empty())
            return true;
        for (auto const* member : members) {
            if (path == StringView(member))
                return true;
        }
        return false;
    };
    if (names_only) {
        for (auto const& entry : reader.value().entries()) {
            if (wanted(entry.path.view()))
                printf("%s%s\n", entry.path.characters(), entry.is_directory && !entry.path.view().ends_with('/') ? "/" : "");
        }
        return 0;
    }
    if (to_output) {
        for (auto const& entry : reader.value().entries()) {
            if (!wanted(entry.path.view()) || entry.is_directory)
                continue;
            auto contents = reader.value().read(entry);
            if (contents.is_error()) {
                fprintf(stderr, "unzip: %s: couldn't read it\n", entry.path.characters());
                return 2;
            }
            fwrite(contents.value().data(), 1, contents.value().size(), stdout);
        }
        return 0;
    }
    if (!list && !test && !quiet)
        printf("Archive:  %s\n", archive_path);
    if (list) {
        printf("Archive:  %s\n  Length      Date    Time    Name\n---------  ---------- -----   ----\n", archive_path);
        unsigned long long total = 0;
        unsigned count = 0;
        for (auto const& entry : reader.value().entries()) {
            if (!wanted(entry.path.view()))
                continue;
            printf("%9u  %02u-%02u-%04u %02u:%02u   %s%s\n", entry.size, (entry.dos_date >> 5) & 15, entry.dos_date & 31, (entry.dos_date >> 9) + 1980,
                entry.dos_time >> 11, (entry.dos_time >> 5) & 63, entry.path.characters(), entry.is_directory ? "/" : "");
            total += entry.size;
            ++count;
        }
        printf("---------                     -------\n%9llu                     %u file%s\n", total, count, count == 1 ? "" : "s");
        return 0;
    }
    if (directory) {
        char with_slash[1024];
        snprintf(with_slash, sizeof(with_slash), "%s/", directory);
        make_parents(with_slash);
        (void)mkdir(directory, 0755);
        if (chdir(directory) != 0) {
            warn("unzip", directory);
            return 1;
        }
    }
    int status = 0;
    for (auto const& entry : reader.value().entries()) {
        if (!wanted(entry.path.view()))
            continue;
        if (!Archive::is_safe_member_path(entry.path.view())) {
            fprintf(stderr, "warning:  skipped \"../\" path component(s) in %s\n", entry.path.characters());
            status = 1;
            continue;
        }
        if (entry.is_directory) {
            if (test)
                continue;
            if (!quiet)
                printf("   creating: %s/\n", entry.path.characters());
            make_parents(entry.path.characters());
            (void)mkdir(entry.path.characters(), entry.mode & 07777);
            continue;
        }
        auto contents = reader.value().read(entry);
        if (contents.is_error()) {
            fprintf(stderr, "  error:  %s: this member is damaged\n", entry.path.characters());
            status = 1;
            continue;
        }
        if (test) {
            if (!quiet)
                printf("    testing: %s   OK\n", entry.path.characters());
            continue;
        }
        if (access(entry.path.characters(), F_OK) == 0 && !overwrite) {
            if (never_overwrite)
                continue;
            fprintf(stderr, "replace %s? use -o to overwrite, -n to leave: skipped\n", entry.path.characters());
            status = 1;
            continue;
        }
        make_parents(entry.path.characters());
        if (entry.is_symbolic_link) {
            auto const target = MUST(String::from_view(StringView(contents.value().span())));
            if (!Archive::is_safe_link_target(entry.path.view(), target.view())) {
                fprintf(stderr, "warning:  skipped a link leading outside of here: %s\n", entry.path.characters());
                status = 1;
                continue;
            }
            (void)unlink(entry.path.characters());
            if (symlink(target.characters(), entry.path.characters()) != 0) {
                warn("unzip", entry.path.characters());
                status = 1;
            } else if (!quiet) {
                printf("    linking: %s\n", entry.path.characters());
            }
            continue;
        }
        if (!quiet)
            printf("  inflating: %s\n", entry.path.characters());
        if (!write_whole_file(entry.path.characters(), contents.value().data(), contents.value().size(), entry.mode & 07777)) {
            warn("unzip", entry.path.characters());
            status = 1;
        }
    }
    if (test && status == 0 && !quiet)
        printf("No errors detected in compressed data of %s.\n", archive_path);
    return status;
}
