/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TreeHelpers.h"
#include <LibBase/Vector.h>
#include <time.h>

// diff [-qrNu] [-U lines] file1 file2: exit status 0 if they're the same, 1 if not, 2 for trouble. The default format is
// ed-like, as POSIX has it; -u is unified. Finding the shortest edit is Myers's algorithm (1986).

namespace {

struct Options {
    bool brief { false };
    bool recursive { false };
    bool new_as_empty { false };
    bool unified { false };
    bool context_format { false };
    int context { 3 };
};
Options g_options;

struct Lines {
    Vector<u8> text;
    // Where each line starts, and one more for where the end is.
    Vector<size_t> starts;
    bool ends_with_newline { true };

    [[nodiscard]] size_t count() const { return starts.size() - 1; }
    [[nodiscard]] StringView line(size_t index) const
    {
        size_t const start = starts[index];
        size_t end = starts[index + 1];
        if (end > start && text[end - 1] == '\n')
            --end;
        return StringView(reinterpret_cast<char const*>(text.data()) + start, end - start);
    }
    [[nodiscard]] bool is_last_without_newline(size_t index) const { return index + 1 == count() && !ends_with_newline; }
};

bool split_lines(Vector<u8> text, Lines& lines)
{
    lines.text = move(text);
    lines.starts.append(0);
    bool binary = false;
    for (size_t i = 0; i < lines.text.size(); ++i) {
        if (lines.text[i] == 0)
            binary = true;
        if (lines.text[i] == '\n')
            lines.starts.append(i + 1);
    }
    lines.ends_with_newline = lines.text.is_empty() || lines.text.last() == '\n';
    if (!lines.ends_with_newline)
        lines.starts.append(lines.text.size());
    return !binary;
}

// A run of lines taken from the first and put in place from the second.
struct Hunk {
    size_t a_start;
    size_t a_length;
    size_t b_start;
    size_t b_length;
};

// Myers: for each number of differences, the furthest along each diagonal it can reach, kept to walk back along at the end.
Vector<Hunk> difference(Lines const& a, Lines const& b)
{
    long const n = static_cast<long>(a.count());
    long const m = static_cast<long>(b.count());
    long const max = n + m;
    Vector<Hunk> hunks;
    if (max == 0)
        return hunks;
    Vector<long> furthest;
    MUST(furthest.try_resize(static_cast<size_t>(2 * max + 2)));
    for (auto& value : furthest)
        value = 0;
    Vector<Vector<long>> history;
    long found_d = -1;
    for (long d = 0; d <= max && found_d < 0; ++d) {
        for (long k = -d; k <= d; k += 2) {
            long x;
            if (k == -d || (k != d && furthest[static_cast<size_t>(k - 1 + max)] < furthest[static_cast<size_t>(k + 1 + max)]))
                x = furthest[static_cast<size_t>(k + 1 + max)];
            else
                x = furthest[static_cast<size_t>(k - 1 + max)] + 1;
            long y = x - k;
            while (x < n && y < m && a.line(static_cast<size_t>(x)) == b.line(static_cast<size_t>(y))) {
                ++x;
                ++y;
            }
            furthest[static_cast<size_t>(k + max)] = x;
            if (x >= n && y >= m) {
                found_d = d;
                break;
            }
        }
        // Only what the next step back needs: this round's diagonals.
        Vector<long> kept;
        long const low = -d - 1;
        long const high = d + 1;
        for (long k = low; k <= high; ++k) {
            size_t const index = static_cast<size_t>(k + max);
            kept.append(index < furthest.size() ? furthest[index] : 0);
        }
        history.append(move(kept));
    }

    // Back from the end, one difference at a time.
    Vector<Hunk> reversed;
    long x = n, y = m;
    for (long d = found_d; d > 0; --d) {
        auto const& previous = history[static_cast<size_t>(d - 1)];
        long const low = -(d - 1) - 1;
        auto at = [&](long k) { return previous[static_cast<size_t>(k - low)]; };
        long const k = x - y;
        long previous_k;
        if (k == -d || (k != d && at(k - 1) < at(k + 1)))
            previous_k = k + 1;
        else
            previous_k = k - 1;
        long const previous_x = at(previous_k);
        long const previous_y = previous_x - previous_k;
        // A deletion or an insertion, after the matching lines it ended on.
        Hunk step {};
        if (previous_k == k + 1) {
            step = { static_cast<size_t>(previous_x), 0, static_cast<size_t>(previous_y), 1 };
        } else {
            step = { static_cast<size_t>(previous_x), 1, static_cast<size_t>(previous_y), 0 };
        }
        reversed.append(step);
        x = previous_x;
        y = previous_y;
    }
    // In order, and neighbours joined.
    for (size_t i = reversed.size(); i > 0; --i) {
        auto const& step = reversed[i - 1];
        if (!hunks.is_empty()) {
            auto& last = hunks.last();
            if (last.a_start + last.a_length == step.a_start && last.b_start + last.b_length == step.b_start) {
                last.a_length += step.a_length;
                last.b_length += step.b_length;
                continue;
            }
        }
        hunks.append(step);
    }
    return hunks;
}

void print_line(char prefix, StringView line, bool no_newline)
{
    putchar(prefix);
    if (prefix == '<' || prefix == '>')
        putchar(' ');
    fwrite(line.characters_without_null_termination(), 1, line.length(), stdout);
    putchar('\n');
    if (no_newline)
        puts("\\ No newline at end of file");
}

void print_line_text(StringView line, bool no_newline)
{
    fwrite(line.characters_without_null_termination(), 1, line.length(), stdout);
    putchar('\n');
    if (no_newline)
        puts("\\ No newline at end of file");
}

void print_range(size_t start, size_t length)
{
    if (length <= 1)
        printf("%zu", length == 0 ? start : start + 1);
    else
        printf("%zu,%zu", start + 1, start + length);
}

void print_normal(Lines const& a, Lines const& b, Vector<Hunk> const& hunks)
{
    for (auto const& hunk : hunks) {
        print_range(hunk.a_start, hunk.a_length);
        putchar(hunk.a_length == 0 ? 'a' : hunk.b_length == 0 ? 'd' : 'c');
        print_range(hunk.b_start, hunk.b_length);
        putchar('\n');
        for (size_t i = 0; i < hunk.a_length; ++i)
            print_line('<', a.line(hunk.a_start + i), a.is_last_without_newline(hunk.a_start + i));
        if (hunk.a_length && hunk.b_length)
            puts("---");
        for (size_t i = 0; i < hunk.b_length; ++i)
            print_line('>', b.line(hunk.b_start + i), b.is_last_without_newline(hunk.b_start + i));
    }
}

void print_time(char const* path)
{
    struct stat information {};
    (void)stat(path, &information);
    tm parts {};
    time_t const when = information.st_mtime;
    localtime_r(&when, &parts);
    char text[64];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &parts);
    printf("%s", text);
}

void print_unified(char const* a_name, char const* b_name, Lines const& a, Lines const& b, Vector<Hunk> const& hunks)
{
    printf("--- %s\t", a_name);
    print_time(a_name);
    printf("\n+++ %s\t", b_name);
    print_time(b_name);
    putchar('\n');
    size_t const context = static_cast<size_t>(g_options.context);
    for (size_t first = 0; first < hunks.size();) {
        // Those close enough to share their context are one block.
        size_t last = first;
        while (last + 1 < hunks.size() && hunks[last + 1].a_start <= hunks[last].a_start + hunks[last].a_length + context * 2)
            ++last;
        size_t const a_start = hunks[first].a_start > context ? hunks[first].a_start - context : 0;
        size_t const b_start = hunks[first].b_start > context ? hunks[first].b_start - context : 0;
        size_t const a_end = min(a.count(), hunks[last].a_start + hunks[last].a_length + context);
        size_t const b_end = min(b.count(), hunks[last].b_start + hunks[last].b_length + context);
        printf("@@ -");
        size_t const a_length = a_end - a_start;
        size_t const b_length = b_end - b_start;
        if (a_length == 1)
            printf("%zu", a_start + 1);
        else
            printf("%zu,%zu", a_length ? a_start + 1 : a_start, a_length);
        printf(" +");
        if (b_length == 1)
            printf("%zu", b_start + 1);
        else
            printf("%zu,%zu", b_length ? b_start + 1 : b_start, b_length);
        printf(" @@\n");
        size_t at = a_start;
        for (size_t index = first; index <= last; ++index) {
            auto const& hunk = hunks[index];
            for (; at < hunk.a_start; ++at)
                print_line(' ', a.line(at), a.is_last_without_newline(at));
            for (size_t i = 0; i < hunk.a_length; ++i)
                print_line('-', a.line(hunk.a_start + i), a.is_last_without_newline(hunk.a_start + i));
            for (size_t i = 0; i < hunk.b_length; ++i)
                print_line('+', b.line(hunk.b_start + i), b.is_last_without_newline(hunk.b_start + i));
            at = hunk.a_start + hunk.a_length;
        }
        for (; at < a_end; ++at)
            print_line(' ', a.line(at), a.is_last_without_newline(at));
        first = last + 1;
    }
}

// -c: each side of a block on its own, with what changed marked: ! for what's in both and different, and + and - for what's in only one.
void print_context(char const* a_name, char const* b_name, Lines const& a, Lines const& b, Vector<Hunk> const& hunks)
{
    printf("*** %s\t", a_name);
    print_time(a_name);
    printf("\n--- %s\t", b_name);
    print_time(b_name);
    putchar('\n');
    size_t const context = static_cast<size_t>(g_options.context);
    auto const print_side_range = [](size_t start, size_t end) {
        if (end - start == 1)
            printf("%zu", start + 1);
        else if (start == end)
            printf("%zu", start);
        else
            printf("%zu,%zu", start + 1, end);
    };
    for (size_t first = 0; first < hunks.size();) {
        size_t last = first;
        while (last + 1 < hunks.size() && hunks[last + 1].a_start <= hunks[last].a_start + hunks[last].a_length + 2 * context)
            ++last;
        size_t const a_start = hunks[first].a_start > context ? hunks[first].a_start - context : 0;
        size_t const b_start = hunks[first].b_start > context ? hunks[first].b_start - context : 0;
        size_t const a_end = min(a.count(), hunks[last].a_start + hunks[last].a_length + context);
        size_t const b_end = min(b.count(), hunks[last].b_start + hunks[last].b_length + context);
        puts("***************");

        bool removes = false, adds = false;
        for (size_t index = first; index <= last; ++index) {
            removes = removes || hunks[index].a_length > 0;
            adds = adds || hunks[index].b_length > 0;
        }

        printf("*** ");
        print_side_range(a_start, a_end);
        puts(" ****");
        if (removes) {
            size_t at = a_start;
            for (size_t index = first; index <= last; ++index) {
                auto const& hunk = hunks[index];
                for (; at < hunk.a_start; ++at) {
                    fputs("  ", stdout);
                    print_line_text(a.line(at), a.is_last_without_newline(at));
                }
                for (size_t i = 0; i < hunk.a_length; ++i) {
                    fputs(hunk.b_length ? "! " : "- ", stdout);
                    print_line_text(a.line(hunk.a_start + i), a.is_last_without_newline(hunk.a_start + i));
                }
                at = hunk.a_start + hunk.a_length;
            }
            for (; at < a_end; ++at) {
                fputs("  ", stdout);
                print_line_text(a.line(at), a.is_last_without_newline(at));
            }
        }

        printf("--- ");
        print_side_range(b_start, b_end);
        puts(" ----");
        if (adds) {
            size_t at = b_start;
            for (size_t index = first; index <= last; ++index) {
                auto const& hunk = hunks[index];
                for (; at < hunk.b_start; ++at) {
                    fputs("  ", stdout);
                    print_line_text(b.line(at), b.is_last_without_newline(at));
                }
                for (size_t i = 0; i < hunk.b_length; ++i) {
                    fputs(hunk.a_length ? "! " : "+ ", stdout);
                    print_line_text(b.line(hunk.b_start + i), b.is_last_without_newline(hunk.b_start + i));
                }
                at = hunk.b_start + hunk.b_length;
            }
            for (; at < b_end; ++at) {
                fputs("  ", stdout);
                print_line_text(b.line(at), b.is_last_without_newline(at));
            }
        }
        first = last + 1;
    }
}

int compare_files(char const* a_path, char const* b_path)
{
    Lines a, b;
    bool a_text = true, b_text = true;
    auto read_one = [](char const* path, Lines& lines, bool& is_text) {
        auto contents = read_whole_file(path);
        if (contents.is_error()) {
            if (g_options.new_as_empty && contents.error().code() == ENOENT) {
                is_text = split_lines({}, lines);
                return true;
            }
            fprintf(stderr, "diff: %s: %s\n", path, strerror(contents.error().code()));
            return false;
        }
        is_text = split_lines(contents.release_value(), lines);
        return true;
    };
    if (!read_one(a_path, a, a_text) || !read_one(b_path, b, b_text))
        return 2;
    if (a.text.size() == b.text.size() && (a.text.is_empty() || memcmp(a.text.data(), b.text.data(), a.text.size()) == 0))
        return 0;
    if (g_options.brief) {
        printf("Files %s and %s differ\n", a_path, b_path);
        return 1;
    }
    if (!a_text || !b_text) {
        printf("Binary files %s and %s differ\n", a_path, b_path);
        return 1;
    }
    auto const hunks = difference(a, b);
    if (g_options.context_format)
        print_context(a_path, b_path, a, b, hunks);
    else if (g_options.unified)
        print_unified(a_path, b_path, a, b, hunks);
    else
        print_normal(a, b, hunks);
    return 1;
}

Vector<String> names_in(char const* directory)
{
    Vector<String> names;
    DIR* listing = opendir(directory);
    if (!listing)
        return names;
    while (auto* entry = readdir(listing)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            names.append(MUST(String::from_view(StringView(entry->d_name))));
    }
    closedir(listing);
    for (size_t i = 1; i < names.size(); ++i) {
        for (size_t j = i; j > 0 && names[j].view() < names[j - 1].view(); --j) {
            auto held = move(names[j]);
            names[j] = move(names[j - 1]);
            names[j - 1] = move(held);
        }
    }
    return names;
}

int compare(char const* a_path, char const* b_path);

int compare_directories(char const* a_path, char const* b_path)
{
    auto const a_names = names_in(a_path);
    auto const b_names = names_in(b_path);
    int status = 0;
    size_t i = 0, j = 0;
    while (i < a_names.size() || j < b_names.size()) {
        bool const take_a = j >= b_names.size() || (i < a_names.size() && a_names[i].view() < b_names[j].view());
        bool const take_b = i >= a_names.size() || (j < b_names.size() && b_names[j].view() < a_names[i].view());
        char a_child[1024], b_child[1024];
        char const* name = take_b ? b_names[j].characters() : a_names[i].characters();
        snprintf(a_child, sizeof(a_child), "%s/%s", a_path, name);
        snprintf(b_child, sizeof(b_child), "%s/%s", b_path, name);
        if ((take_a || take_b) && !g_options.new_as_empty) {
            printf("Only in %s: %s\n", take_a ? a_path : b_path, name);
            status = max(status, 1);
        } else if (g_options.recursive || (take_a || take_b)) {
            status = max(status, compare(a_child, b_child));
        } else {
            struct stat a_status {}, b_status {};
            (void)stat(a_child, &a_status);
            (void)stat(b_child, &b_status);
            if (S_ISDIR(a_status.st_mode) && S_ISDIR(b_status.st_mode))
                printf("Common subdirectories: %s and %s\n", a_child, b_child);
            else
                status = max(status, compare(a_child, b_child));
        }
        if (!take_b)
            ++i;
        if (!take_a)
            ++j;
    }
    return status;
}

int compare(char const* a_path, char const* b_path)
{
    struct stat a_status {}, b_status {};
    bool const a_exists = stat(a_path, &a_status) == 0;
    bool const b_exists = stat(b_path, &b_status) == 0;
    if (!a_exists && !g_options.new_as_empty && strcmp(a_path, "-") != 0) {
        fprintf(stderr, "diff: %s: %s\n", a_path, strerror(ENOENT));
        return 2;
    }
    if (!b_exists && !g_options.new_as_empty && strcmp(b_path, "-") != 0) {
        fprintf(stderr, "diff: %s: %s\n", b_path, strerror(ENOENT));
        return 2;
    }
    bool const a_directory = a_exists && S_ISDIR(a_status.st_mode);
    bool const b_directory = b_exists && S_ISDIR(b_status.st_mode);
    if (a_directory && b_directory)
        return compare_directories(a_path, b_path);
    // A directory and a file: the file of that name in the directory.
    if (a_directory != b_directory && a_exists && b_exists) {
        char joined[1024];
        snprintf(joined, sizeof(joined), "%s/%s", a_directory ? a_path : b_path, last_component(a_directory ? b_path : a_path));
        return a_directory ? compare_files(joined, b_path) : compare_files(a_path, joined);
    }
    return compare_files(a_path, b_path);
}

int usage()
{
    fprintf(stderr, "usage: diff [-cqrNu] [-U lines] file1 file2\n");
    return 2;
}

}

int main(int argc, char** argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "--") == 0) {
            ++first;
            break;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            switch (*option) {
            case 'q':
                g_options.brief = true;
                break;
            case 'r':
                g_options.recursive = true;
                break;
            case 'N':
                g_options.new_as_empty = true;
                break;
            case 'c':
                g_options.context_format = true;
                break;
            case 'u':
                g_options.unified = true;
                break;
            case 'U': {
                char const* value = option[1] ? option + 1 : (first + 1 < argc ? argv[++first] : nullptr);
                if (!value)
                    return usage();
                g_options.unified = true;
                g_options.context = atoi(value);
                option = "x";
                break;
            }
            case 'a':
                break;
            default:
                return usage();
            }
        }
    }
    if (argc - first != 2)
        return usage();
    return compare(argv[first], argv[first + 1]);
}
