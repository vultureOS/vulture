/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TreeHelpers.h"
#include <LibArchive/Tar.h>
#include <LibCompress/Deflate.h>

// tar -c|-x|-t [-vzpkP] [-f archive] [-C directory] [file...], as bsdtar has it on a Mac: the options may run together, with
// or without a dash ("tar czf a.tgz b"). gzip is noticed by itself on the way in.

namespace {

class VectorSink final : public Archive::TarSink {
public:
    void write(ReadonlyBytes bytes) override
    {
        size_t const at = data.size();
        MUST(data.try_resize(at + bytes.size()));
        if (!bytes.is_empty())
            memcpy(data.data() + at, bytes.data(), bytes.size());
    }
    Vector<u8> data;
};

void print_long(FILE* out, char type, unsigned mode, unsigned long long size, char const* name, char const* link)
{
    char permissions[11] = "----------";
    permissions[0] = type;
    char const letters[] = "rwxrwxrwx";
    for (int bit = 0; bit < 9; ++bit) {
        if (mode & (0400u >> bit))
            permissions[bit + 1] = letters[bit];
    }
    fprintf(out, "%s  0 0      0   %10llu %s%s%s\n", permissions, size, name, link ? " -> " : "", link ? link : "");
}

int usage()
{
    fprintf(stderr, "usage: tar {-c|-x|-t} [-vzpkP] [-f archive] [-C directory] [file ...]\n");
    return 1;
}

}

int main(int argc, char** argv)
{
    char mode = 0;
    bool verbose = false, gzip = false, keep_old = false, absolute = false;
    char const* archive = nullptr;
    char const* directory = nullptr;
    Vector<char const*> names;

    for (int i = 1; i < argc; ++i) {
        char const* argument = argv[i];
        // The first argument's options don't need their dash.
        bool const options = argument[0] == '-' ? argument[1] != 0 : i == 1;
        if (!options) {
            names.append(argument);
            continue;
        }
        if (argument[0] == '-' && argument[1] == '-') {
            fprintf(stderr, "tar: unknown option %s\n", argument);
            return usage();
        }
        for (char const* option = argument[0] == '-' ? argument + 1 : argument; *option; ++option) {
            switch (*option) {
            case 'c':
            case 'x':
            case 't':
                mode = *option;
                break;
            case 'v':
                verbose = true;
                break;
            case 'z':
                gzip = true;
                break;
            case 'p':
            case 'o':
                break;
            case 'k':
                keep_old = true;
                break;
            case 'P':
                absolute = true;
                break;
            case 'f':
            case 'C': {
                bool const is_file = *option == 'f';
                char const* value = option[1] ? option + 1 : (i + 1 < argc ? argv[++i] : nullptr);
                if (!value)
                    return usage();
                (is_file ? archive : directory) = value;
                goto next_argument;
            }
            default:
                fprintf(stderr, "tar: unknown option -- %c\n", *option);
                return usage();
            }
        }
    next_argument:;
    }
    if (!mode)
        return usage();
    if (!archive)
        archive = "-";

    if (mode == 'c') {
        if (names.is_empty()) {
            fprintf(stderr, "tar: no files or directories specified\n");
            return 1;
        }
        // The archive's where it was asked to be, wherever what goes in it is.
        int const started_in = open(".", O_RDONLY | O_CLOEXEC);
        if (directory && chdir(directory) != 0) {
            warn("tar", directory);
            return 1;
        }
        VectorSink sink;
        Archive::TarWriter writer(sink);
        int status = 0;
        for (auto const* name : names) {
            struct stat there;
            if (lstat(name, &there) != 0) {
                fprintf(stderr, "tar: %s: Cannot stat: %s\n", name, strerror(errno));
                status = 1;
                continue;
            }
            StringView member { name };
            while (!absolute && member.starts_with('/'))
                member = member.substring_view(1);
            auto const base = MUST(String::from_view(member));
            walk_tree(name, base.characters(), [&](char const* relative, char const* full, struct stat const& information) {
                // A file can be its own name, or a tree's.
                auto problem = [&](auto const& result) {
                    if (result.is_error()) {
                        fprintf(stderr, "tar: %s: too long, or not a kind of thing this can keep\n", relative);
                        status = 1;
                    }
                };
                if (verbose)
                    fprintf(stderr, "a %s\n", relative);
                if (S_ISDIR(information.st_mode)) {
                    problem(writer.add_directory(StringView(relative), information.st_mode & 07777));
                } else if (S_ISLNK(information.st_mode)) {
                    char target[1024];
                    ssize_t const length = readlink(full, target, sizeof(target) - 1);
                    target[length > 0 ? length : 0] = 0;
                    problem(writer.add_symbolic_link(StringView(relative), StringView(target)));
                } else if (S_ISREG(information.st_mode)) {
                    auto contents = read_whole_file(full);
                    if (contents.is_error()) {
                        fprintf(stderr, "tar: %s: %s\n", full, strerror(contents.error().code()));
                        status = 1;
                        return;
                    }
                    problem(writer.add_file(StringView(relative), information.st_mode & 07777, contents.value().span()));
                }
            });
        }
        writer.finish();
        Vector<u8> output = move(sink.data);
        // "tar -czf x.tgz", or a name that says so: bsdtar's -a is the only way it'd guess, but gzip was asked for here.
        if (gzip) {
            auto compressed = Compress::gzip_compress(output.span());
            if (compressed.is_error()) {
                fprintf(stderr, "tar: couldn't compress the archive\n");
                return 1;
            }
            output = compressed.release_value();
        }
        if (started_in >= 0)
            (void)fchdir(started_in);
        int const out = strcmp(archive, "-") == 0 ? STDOUT_FILENO : open(archive, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0 || !write_all_to(out, output.data(), output.size())) {
            warn("tar", archive);
            return 1;
        }
        if (status)
            fprintf(stderr, "tar: Error exit delayed from previous errors.\n");
        return status;
    }

    auto raw = read_whole_file(archive);
    if (raw.is_error()) {
        fprintf(stderr, "tar: Error opening archive: Failed to open '%s'\n", archive);
        return 1;
    }
    Vector<u8> contents = raw.release_value();
    if (contents.size() >= 2 && contents[0] == 0x1f && contents[1] == 0x8b) {
        auto expanded = Compress::gzip_decompress(contents.span(), 1ull << 34);
        if (expanded.is_error()) {
            fprintf(stderr, "tar: Error opening archive: this gzip data is damaged\n");
            return 1;
        }
        contents = expanded.release_value();
    }
    auto reader = Archive::TarReader::parse(contents.span());
    if (reader.is_error()) {
        fprintf(stderr, "tar: Error opening archive: Unrecognized archive format\n");
        return 1;
    }
    if (directory && chdir(directory) != 0) {
        warn("tar", directory);
        return 1;
    }
    auto wanted = [&](StringView path) {
        if (names.is_empty())
            return true;
        for (auto const* name : names) {
            StringView const prefix { name };
            if (path == prefix || (path.starts_with(prefix) && path.length() > prefix.length() && path[prefix.length()] == '/'))
                return true;
        }
        return false;
    };
    int status = 0;
    auto result = reader.value().for_each_entry([&](Archive::TarEntry const& entry) {
        if (!wanted(entry.path))
            return Archive::IterationDecision::Continue;
        auto const path = MUST(String::from_view(entry.path));
        if (mode == 't') {
            if (verbose) {
                char const type = entry.type == Archive::TarEntryType::Directory ? 'd' : entry.type == Archive::TarEntryType::SymbolicLink ? 'l' : '-';
                auto const link = MUST(String::from_view(entry.link_target));
                print_long(stdout, type, entry.mode, entry.data.size(), path.characters(), entry.type == Archive::TarEntryType::SymbolicLink ? link.characters() : nullptr);
            } else {
                printf("%s%s\n", path.characters(), entry.type == Archive::TarEntryType::Directory ? "/" : "");
            }
            return Archive::IterationDecision::Continue;
        }
        // What's unpacked stays where it's put.
        if (!absolute && !Archive::is_safe_member_path(entry.path)) {
            fprintf(stderr, "tar: %s: Path contains '..'\n", path.characters());
            status = 1;
            return Archive::IterationDecision::Continue;
        }
        if (verbose)
            fprintf(stderr, "x %s\n", path.characters());
        make_parents(path.characters());
        if (keep_old && access(path.characters(), F_OK) == 0)
            return Archive::IterationDecision::Continue;
        switch (entry.type) {
        case Archive::TarEntryType::Directory:
            if (mkdir(path.characters(), entry.mode & 07777) != 0 && errno != EEXIST) {
                warn("tar", path.characters());
                status = 1;
            }
            break;
        case Archive::TarEntryType::SymbolicLink: {
            if (!absolute && !Archive::is_safe_link_target(entry.path, entry.link_target)) {
                fprintf(stderr, "tar: %s: a link leading outside of here\n", path.characters());
                status = 1;
                break;
            }
            auto const target = MUST(String::from_view(entry.link_target));
            (void)unlink(path.characters());
            if (symlink(target.characters(), path.characters()) != 0) {
                warn("tar", path.characters());
                status = 1;
            }
            break;
        }
        case Archive::TarEntryType::File:
            if (!write_whole_file(path.characters(), entry.data.data(), entry.data.size(), entry.mode & 07777)) {
                warn("tar", path.characters());
                status = 1;
            }
            break;
        default:
            break;
        }
        return Archive::IterationDecision::Continue;
    });
    if (result.is_error()) {
        fprintf(stderr, "tar: this archive is damaged\n");
        return 1;
    }
    return status;
}
