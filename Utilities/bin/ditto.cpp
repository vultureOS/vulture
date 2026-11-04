/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TreeHelpers.h"
#include <LibArchive/Tar.h>
#include <LibArchive/Zip.h>
#include <sys/stat.h>

// ditto [-V] src... dst: copies files and whole trees, keeping modes and links, merging into what's there.
// ditto -c -k [--keepParent] src archive.zip, and ditto -x -k archive.zip dst: the same, to and from a zip archive.

namespace {

bool g_verbose = false;

bool copy_one(char const* from, char const* to, struct stat const& status)
{
    if (g_verbose)
        fprintf(stderr, "copying file %s\n", to);
    if (S_ISDIR(status.st_mode)) {
        if (mkdir(to, status.st_mode & 07777) != 0 && errno != EEXIST) {
            warn("ditto", to);
            return false;
        }
        return true;
    }
    if (S_ISLNK(status.st_mode)) {
        char target[1024];
        ssize_t const length = readlink(from, target, sizeof(target) - 1);
        target[length > 0 ? length : 0] = 0;
        (void)unlink(to);
        if (symlink(target, to) != 0) {
            warn("ditto", to);
            return false;
        }
        return true;
    }
    if (!S_ISREG(status.st_mode))
        return true;
    auto contents = read_whole_file(from);
    if (contents.is_error()) {
        fprintf(stderr, "ditto: %s: %s\n", from, strerror(contents.error().code()));
        return false;
    }
    if (!write_whole_file(to, contents.value().data(), contents.value().size(), status.st_mode & 07777)) {
        warn("ditto", to);
        return false;
    }
    return true;
}

bool copy_tree(char const* source, char const* destination)
{
    bool ok = true;
    walk_tree(source, "", [&](char const* relative, char const* full, struct stat const& status) {
        char target[1024];
        if (relative[0])
            snprintf(target, sizeof(target), "%s/%s", destination, relative);
        else
            snprintf(target, sizeof(target), "%s", destination);
        make_parents(target);
        ok = copy_one(full, target, status) && ok;
    });
    return ok;
}

int make_archive(char const* source, char const* archive, bool keep_parent)
{
    Archive::ZipWriter writer;
    int status = 0;
    char const* parent = keep_parent ? last_component(source) : "";
    char prefix[1024];
    snprintf(prefix, sizeof(prefix), "%s", parent);
    walk_tree(source, prefix, [&](char const* relative, char const* full, struct stat const& information) {
        if (!relative[0])
            return;
        u16 date = 0, clock = 0;
        to_dos_time(information.st_mtime, date, clock);
        StringView const member { relative };
        if (S_ISDIR(information.st_mode)) {
            MUST(writer.add_directory(member, information.st_mode, date, clock));
        } else if (S_ISLNK(information.st_mode)) {
            char target[1024];
            ssize_t const length = readlink(full, target, sizeof(target) - 1);
            target[length > 0 ? length : 0] = 0;
            MUST(writer.add_symbolic_link(member, StringView(target), date, clock));
        } else if (S_ISREG(information.st_mode)) {
            auto contents = read_whole_file(full);
            if (contents.is_error()) {
                fprintf(stderr, "ditto: %s: %s\n", full, strerror(contents.error().code()));
                status = 1;
                return;
            }
            MUST(writer.add_file(member, information.st_mode, contents.value().span(), date, clock));
        }
    });
    auto finished = writer.finish();
    if (finished.is_error() || !write_whole_file(archive, finished.value().data(), finished.value().size(), 0644)) {
        warn("ditto", archive);
        return 1;
    }
    return status;
}

int extract_archive(char const* archive, char const* destination)
{
    auto bytes = read_whole_file(archive);
    if (bytes.is_error()) {
        fprintf(stderr, "ditto: %s: %s\n", archive, strerror(bytes.error().code()));
        return 1;
    }
    auto reader = Archive::ZipReader::parse(bytes.value().span());
    if (reader.is_error()) {
        fprintf(stderr, "ditto: %s: this isn't a zip archive\n", archive);
        return 1;
    }
    int status = 0;
    for (auto const& entry : reader.value().entries()) {
        if (!Archive::is_safe_member_path(entry.path.view())) {
            fprintf(stderr, "ditto: skipping %s: it leads outside of here\n", entry.path.characters());
            status = 1;
            continue;
        }
        char target[1024];
        snprintf(target, sizeof(target), "%s/%s", destination, entry.path.characters());
        make_parents(target);
        if (entry.is_directory) {
            (void)mkdir(target, entry.mode & 07777);
            continue;
        }
        auto contents = reader.value().read(entry);
        if (contents.is_error()) {
            fprintf(stderr, "ditto: %s: this member is damaged\n", entry.path.characters());
            status = 1;
            continue;
        }
        if (entry.is_symbolic_link) {
            auto const link_target = MUST(String::from_view(StringView(contents.value().span())));
            if (!Archive::is_safe_link_target(entry.path.view(), link_target.view())) {
                fprintf(stderr, "ditto: skipping %s: a link leading outside of here\n", entry.path.characters());
                status = 1;
                continue;
            }
            (void)unlink(target);
            if (symlink(link_target.characters(), target) != 0) {
                warn("ditto", target);
                status = 1;
            }
            continue;
        }
        if (!write_whole_file(target, contents.value().data(), contents.value().size(), entry.mode & 07777)) {
            warn("ditto", target);
            status = 1;
        }
    }
    return status;
}

int usage()
{
    fprintf(stderr, "Usage: ditto [-V] src ... dst\n       ditto -c -k [--keepParent] src archive\n       ditto -x -k archive dst\n");
    return 1;
}

}

int main(int argc, char** argv)
{
    bool create = false, extract = false, zip = false, keep_parent = false;
    Vector<char const*> paths;
    for (int i = 1; i < argc; ++i) {
        char const* argument = argv[i];
        if (strcmp(argument, "--keepParent") == 0)
            keep_parent = true;
        else if (strcmp(argument, "--rsrc") == 0 || strcmp(argument, "--norsrc") == 0 || strcmp(argument, "--sequesterRsrc") == 0)
            continue;
        else if (argument[0] == '-' && argument[1]) {
            for (char const* option = argument + 1; *option; ++option) {
                switch (*option) {
                case 'c':
                    create = true;
                    break;
                case 'x':
                    extract = true;
                    break;
                case 'k':
                    zip = true;
                    break;
                case 'V':
                case 'v':
                    g_verbose = true;
                    break;
                default:
                    return usage();
                }
            }
        } else {
            paths.append(argument);
        }
    }
    if (create || extract) {
        if (!zip) {
            fprintf(stderr, "ditto: only zip archives (-k) are here so far\n");
            return 1;
        }
        if (paths.size() != 2)
            return usage();
        return create ? make_archive(paths[0], paths[1], keep_parent) : extract_archive(paths[0], paths[1]);
    }
    if (paths.size() < 2)
        return usage();
    char const* destination = paths.last();
    struct stat information;
    bool const destination_is_directory = stat(destination, &information) == 0 && S_ISDIR(information.st_mode);
    if (paths.size() > 2 && !destination_is_directory) {
        fprintf(stderr, "ditto: %s: Not a directory\n", destination);
        return 1;
    }
    bool ok = true;
    for (size_t i = 0; i + 1 < paths.size(); ++i) {
        struct stat source;
        if (lstat(paths[i], &source) != 0) {
            fprintf(stderr, "ditto: %s: %s\n", paths[i], strerror(errno));
            ok = false;
            continue;
        }
        // A directory's contents go into the destination, unlike cp -R. A file goes in it by name.
        char target[1024];
        if (!S_ISDIR(source.st_mode) && destination_is_directory)
            snprintf(target, sizeof(target), "%s/%s", destination, last_component(paths[i]));
        else
            snprintf(target, sizeof(target), "%s", destination);
        ok = copy_tree(paths[i], target) && ok;
    }
    return ok ? 0 : 1;
}
