/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TreeHelpers.h"

// patch [-Rs] [--dry-run] [-p N] [-i patchfile] [-d directory] [file]: applies unified or plain diffs. A hunk may have moved
// up or down the file a little: it's looked for nearby. One that isn't found goes in file.rej, and the status is 1.

namespace {

struct Hunk {
    size_t old_start { 0 };
    Vector<String> old_lines;
    Vector<String> new_lines;
    Vector<String> original_text;
    bool old_ends_without_newline { false };
    bool new_ends_without_newline { false };
};

struct FilePatch {
    String old_name;
    String new_name;
    Vector<Hunk> hunks;
};

Vector<String> split(StringView text, bool& ends_with_newline)
{
    Vector<String> lines;
    size_t start = 0;
    while (start < text.length()) {
        auto const newline = text.find('\n', start);
        size_t const end = newline.has_value() ? newline.value() : text.length();
        lines.append(MUST(String::from_view(text.substring_view(start, end - start))));
        start = end + 1;
    }
    ends_with_newline = text.is_empty() || text.ends_with('\n');
    return lines;
}

// "a/b/c" with -p1 is "b/c". Without -p, the last part of it, as POSIX's patch has it.
String strip_path(StringView name, int strip)
{
    if (strip < 0) {
        auto const slash = name.find_last('/');
        return MUST(String::from_view(slash.has_value() ? name.substring_view(slash.value() + 1) : name));
    }
    while (strip-- > 0) {
        auto const slash = name.find('/');
        if (!slash.has_value())
            break;
        name = name.substring_view(slash.value() + 1);
    }
    return MUST(String::from_view(name));
}

// "--- name\tdate": just the name.
StringView header_name(StringView line)
{
    line = line.substring_view(4);
    auto const tab = line.find('\t');
    if (tab.has_value())
        line = line.substring_view(0, tab.value());
    return line.trim_whitespace();
}

Vector<FilePatch> parse(Vector<String> const& lines, String const& only_file)
{
    Vector<FilePatch> patches;
    size_t i = 0;
    auto in_plain_hunk_header = [](StringView line, size_t& a1, size_t& a2, char& kind, size_t& b1, size_t& b2) {
        // 3,4c5,6 or 3a4 or 3d2
        size_t at = 0;
        auto number = [&](size_t& out) {
            if (at >= line.length() || line[at] < '0' || line[at] > '9')
                return false;
            out = 0;
            while (at < line.length() && line[at] >= '0' && line[at] <= '9')
                out = out * 10 + static_cast<size_t>(line[at++] - '0');
            return true;
        };
        if (!number(a1))
            return false;
        a2 = a1;
        if (at < line.length() && line[at] == ',') {
            ++at;
            if (!number(a2))
                return false;
        }
        if (at >= line.length() || (line[at] != 'a' && line[at] != 'c' && line[at] != 'd'))
            return false;
        kind = line[at++];
        if (!number(b1))
            return false;
        b2 = b1;
        if (at < line.length() && line[at] == ',') {
            ++at;
            if (!number(b2))
                return false;
        }
        return at == line.length();
    };
    while (i < lines.size()) {
        auto const line = lines[i].view();
        if (line.starts_with("--- "_sv) && i + 1 < lines.size() && lines[i + 1].view().starts_with("+++ "_sv)) {
            FilePatch patch;
            patch.old_name = MUST(String::from_view(header_name(line)));
            patch.new_name = MUST(String::from_view(header_name(lines[i + 1].view())));
            i += 2;
            while (i < lines.size() && lines[i].view().starts_with("@@ "_sv)) {
                Hunk hunk;
                auto const header = lines[i].view();
                hunk.original_text.append(lines[i]);
                // @@ -a,b +c,d @@
                size_t at = 4;
                size_t old_start = 0;
                while (at < header.length() && header[at] >= '0' && header[at] <= '9')
                    old_start = old_start * 10 + static_cast<size_t>(header[at++] - '0');
                size_t old_count = 1;
                if (at < header.length() && header[at] == ',') {
                    ++at;
                    old_count = 0;
                    while (at < header.length() && header[at] >= '0' && header[at] <= '9')
                        old_count = old_count * 10 + static_cast<size_t>(header[at++] - '0');
                }
                while (at < header.length() && header[at] != '+')
                    ++at;
                ++at;
                while (at < header.length() && header[at] >= '0' && header[at] <= '9')
                    ++at;
                size_t new_count = 1;
                if (at < header.length() && header[at] == ',') {
                    ++at;
                    new_count = 0;
                    while (at < header.length() && header[at] >= '0' && header[at] <= '9')
                        new_count = new_count * 10 + static_cast<size_t>(header[at++] - '0');
                }
                hunk.old_start = old_count == 0 ? old_start : old_start - 1;
                ++i;
                size_t old_seen = 0, new_seen = 0;
                while (i < lines.size() && (old_seen < old_count || new_seen < new_count || lines[i].view().starts_with("\\"_sv))) {
                    auto const body = lines[i].view();
                    hunk.original_text.append(lines[i]);
                    if (body.starts_with("\\"_sv)) {
                        // "\ No newline at end of file": of what's just gone by.
                        if (i > 0 && lines[i - 1].view().starts_with("+"_sv))
                            hunk.new_ends_without_newline = true;
                        else if (i > 0 && lines[i - 1].view().starts_with("-"_sv))
                            hunk.old_ends_without_newline = true;
                        else {
                            hunk.new_ends_without_newline = true;
                            hunk.old_ends_without_newline = true;
                        }
                        ++i;
                        continue;
                    }
                    auto const text = MUST(String::from_view(body.is_empty() ? StringView {} : body.substring_view(1)));
                    if (body.is_empty() || body.starts_with(" "_sv)) {
                        hunk.old_lines.append(text);
                        hunk.new_lines.append(text);
                        ++old_seen;
                        ++new_seen;
                    } else if (body.starts_with("-"_sv)) {
                        hunk.old_lines.append(text);
                        ++old_seen;
                    } else if (body.starts_with("+"_sv)) {
                        hunk.new_lines.append(text);
                        ++new_seen;
                    } else {
                        break;
                    }
                    ++i;
                }
                patch.hunks.append(move(hunk));
            }
            patches.append(move(patch));
            continue;
        }
        size_t a1 = 0, a2 = 0, b1 = 0, b2 = 0;
        char kind = 0;
        if (in_plain_hunk_header(line, a1, a2, kind, b1, b2)) {
            if (patches.is_empty() || !patches.last().old_name.view().is_empty()) {
                FilePatch patch;
                patches.append(move(patch));
            }
            Hunk hunk;
            hunk.original_text.append(lines[i]);
            hunk.old_start = kind == 'a' ? a1 : a1 - 1;
            ++i;
            while (i < lines.size() && lines[i].view().starts_with("< "_sv)) {
                hunk.original_text.append(lines[i]);
                hunk.old_lines.append(MUST(String::from_view(lines[i].view().substring_view(2))));
                ++i;
            }
            if (i < lines.size() && lines[i].view() == "---"_sv) {
                hunk.original_text.append(lines[i]);
                ++i;
            }
            while (i < lines.size() && lines[i].view().starts_with("> "_sv)) {
                hunk.original_text.append(lines[i]);
                hunk.new_lines.append(MUST(String::from_view(lines[i].view().substring_view(2))));
                ++i;
            }
            patches.last().hunks.append(move(hunk));
            continue;
        }
        ++i;
    }
    if (!only_file.view().is_empty()) {
        for (auto& patch : patches) {
            patch.old_name = MUST(String::from_view(only_file.view()));
            patch.new_name = MUST(String::from_view(only_file.view()));
        }
    }
    return patches;
}

bool matches_at(Vector<String> const& lines, Vector<String> const& wanted, size_t at)
{
    if (at + wanted.size() > lines.size())
        return false;
    for (size_t i = 0; i < wanted.size(); ++i) {
        if (lines[at + i].view() != wanted[i].view())
            return false;
    }
    return true;
}

}

int main(int argc, char** argv)
{
    int strip = -1;
    bool reverse = false, silent = false, dry_run = false;
    char const* patch_file = nullptr;
    char const* directory = nullptr;
    String only_file;
    for (int i = 1; i < argc; ++i) {
        char const* argument = argv[i];
        if (strcmp(argument, "--dry-run") == 0 || strcmp(argument, "-C") == 0) {
            dry_run = true;
        } else if (strncmp(argument, "-p", 2) == 0) {
            strip = atoi(argument[2] ? argument + 2 : (i + 1 < argc ? argv[++i] : "0"));
        } else if (strcmp(argument, "-i") == 0 && i + 1 < argc) {
            patch_file = argv[++i];
        } else if (strcmp(argument, "-d") == 0 && i + 1 < argc) {
            directory = argv[++i];
        } else if (strcmp(argument, "-R") == 0) {
            reverse = true;
        } else if (strcmp(argument, "-s") == 0 || strcmp(argument, "-f") == 0 || strcmp(argument, "-N") == 0) {
            silent = silent || argument[1] == 's';
        } else if (argument[0] == '-' && argument[1]) {
            fprintf(stderr, "usage: patch [-Rs] [--dry-run] [-p N] [-i patchfile] [-d directory] [file]\n");
            return 2;
        } else if (only_file.is_empty()) {
            only_file = MUST(String::from_view(StringView(argument)));
        } else {
            // patch [origfile [patchfile]]
            patch_file = argument;
        }
    }
    auto patch_data = read_whole_file(patch_file ? patch_file : "-");
    if (patch_data.is_error()) {
        fprintf(stderr, "patch: can't read the patch: %s\n", strerror(patch_data.error().code()));
        return 2;
    }
    if (directory && chdir(directory) != 0) {
        warn("patch", directory);
        return 2;
    }
    bool ignored = false;
    auto const patch_lines = split(StringView(patch_data.value().span()), ignored);
    auto patches = parse(patch_lines, only_file);
    if (patches.is_empty()) {
        fprintf(stderr, "patch: **** Only garbage was found in the patch input.\n");
        return 2;
    }
    int status = 0;
    for (auto& patch : patches) {
        // Whichever of the two names the file has, unless it's /dev/null: a new file.
        bool const creating = patch.old_name.view() == "/dev/null"_sv;
        bool const removing = patch.new_name.view() == "/dev/null"_sv;
        auto target = strip_path((creating ? patch.new_name : patch.old_name).view(), strip);
        if (!creating && access(target.characters(), F_OK) != 0) {
            auto other = strip_path(patch.new_name.view(), strip);
            if (access(other.characters(), F_OK) == 0)
                target = move(other);
        }
        if (target.view().is_empty()) {
            fprintf(stderr, "patch: which file is to be patched?\n");
            return 2;
        }
        Vector<String> lines;
        bool ends_with_newline = true;
        mode_t mode = 0644;
        struct stat information {};
        if (stat(target.characters(), &information) == 0) {
            mode = information.st_mode & 07777;
            auto contents = read_whole_file(target.characters());
            if (contents.is_error()) {
                fprintf(stderr, "patch: %s: %s\n", target.characters(), strerror(contents.error().code()));
                status = 2;
                continue;
            }
            lines = split(StringView(contents.value().span()), ends_with_newline);
        } else if (!creating && !reverse) {
            fprintf(stderr, "patch: %s: %s\n", target.characters(), strerror(errno));
            status = 2;
            continue;
        }
        if (!silent)
            printf("patching file %s\n", target.characters());
        Vector<String> rejected;
        long drift = 0;
        size_t number = 0;
        bool final_without_newline = !ends_with_newline;
        for (auto& hunk : patch.hunks) {
            ++number;
            auto const& take = reverse ? hunk.new_lines : hunk.old_lines;
            auto const& put = reverse ? hunk.old_lines : hunk.new_lines;
            long const wanted = static_cast<long>(hunk.old_start) + drift;
            long found = -1;
            // As near as can be to where it says.
            for (long distance = 0; distance <= static_cast<long>(lines.size()) + 1 && found < 0; ++distance) {
                long const candidates[] = { wanted - distance, wanted + distance };
                for (long const candidate : candidates) {
                    if (candidate >= 0 && matches_at(lines, take, static_cast<size_t>(candidate))) {
                        found = candidate;
                        break;
                    }
                }
            }
            if (found < 0) {
                if (!silent)
                    printf("Hunk #%zu FAILED at %zu.\n", number, hunk.old_start + 1);
                for (auto const& text : hunk.original_text)
                    rejected.append(MUST(String::from_view(text.view())));
                status = 1;
                continue;
            }
            if (found != wanted && !silent)
                printf("Hunk #%zu succeeded at %ld (offset %ld line%s).\n", number, found + 1, found - wanted, found - wanted == 1 || found - wanted == -1 ? "" : "s");
            Vector<String> result;
            for (long i = 0; i < found; ++i)
                result.append(move(lines[static_cast<size_t>(i)]));
            for (auto const& text : put)
                result.append(MUST(String::from_view(text.view())));
            for (size_t i = static_cast<size_t>(found) + take.size(); i < lines.size(); ++i)
                result.append(move(lines[i]));
            lines = move(result);
            drift += static_cast<long>(put.size()) - static_cast<long>(take.size());
            bool const at_end = static_cast<size_t>(found) + put.size() == lines.size();
            if (at_end && (reverse ? hunk.old_ends_without_newline : hunk.new_ends_without_newline))
                final_without_newline = true;
            else if (at_end)
                final_without_newline = false;
        }
        if (dry_run)
            continue;
        if (removing && lines.is_empty() && !reverse) {
            (void)unlink(target.characters());
        } else {
            StringBuilder output;
            for (size_t i = 0; i < lines.size(); ++i) {
                output.append(lines[i].view());
                if (i + 1 < lines.size() || !final_without_newline)
                    output.append('\n');
            }
            make_parents(target.characters());
            if (!write_whole_file(target.characters(), output.view().characters_without_null_termination(), output.view().length(), mode)) {
                warn("patch", target.characters());
                status = 2;
            }
        }
        if (!rejected.is_empty()) {
            char reject[1024];
            snprintf(reject, sizeof(reject), "%s.rej", target.characters());
            StringBuilder output;
            for (auto const& text : rejected) {
                output.append(text.view());
                output.append('\n');
            }
            (void)write_whole_file(reject, output.view().characters_without_null_termination(), output.view().length(), 0644);
            if (!silent)
                printf("%zu out of %zu hunks FAILED -- saving rejects to file %s\n", rejected.size() ? number - 0 : 0, patch.hunks.size(), reject);
        }
    }
    return status;
}
