/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TreeHelpers.h"
#include <sys/ioctl.h>

// man [section] name: finds name.section under /usr/share/man (or $MANPATH), sets it in the man(7) macros' way for this
// terminal, and shows it with the pager. Also man -w name, for where it is.

namespace {

struct Formatter {
    StringBuilder out;
    int width { 78 };
    int column { 0 };
    int indent { 7 };
    int pending_indent { -1 };
    bool filling { true };
    bool tty { false };

    void newline()
    {
        out.append('\n');
        column = 0;
    }

    void blank_line()
    {
        if (column != 0)
            newline();
        // No two in a row.
        auto const text = out.view();
        if (text.length() >= 2 && text[text.length() - 1] == '\n' && text[text.length() - 2] == '\n')
            return;
        newline();
    }

    void start_line_if_needed()
    {
        if (column != 0)
            return;
        int const at = pending_indent >= 0 ? pending_indent : indent;
        pending_indent = -1;
        for (int i = 0; i < at; ++i)
            out.append(' ');
        column = at;
    }

    void add_word(StringView word, bool bold, bool underline)
    {
        size_t visible = 0;
        for (char const c : word) {
            if ((static_cast<unsigned char>(c) & 0xc0) != 0x80)
                ++visible;
        }
        if (column != 0 && column + 1 + static_cast<int>(visible) > width)
            newline();
        if (column != 0 && column != (pending_indent >= 0 ? pending_indent : indent)) {
            out.append(' ');
            ++column;
        }
        start_line_if_needed();
        if (tty && bold)
            out.append("\x1b[1m"_sv);
        if (tty && underline)
            out.append("\x1b[4m"_sv);
        out.append(word);
        if (tty && (bold || underline))
            out.append("\x1b[0m"_sv);
        column += static_cast<int>(visible);
    }

    // Text, with \fB and \fI switching fonts, and the few escapes there are.
    void add_text(StringView text, bool bold, bool underline)
    {
        StringBuilder word;
        auto flush = [&] {
            if (!word.view().is_empty()) {
                add_word(word.view(), bold, underline);
                word.clear();
            }
        };
        for (size_t i = 0; i < text.length(); ++i) {
            char const c = text[i];
            if (c == '\\' && i + 1 < text.length()) {
                char const e = text[++i];
                if (e == 'f' && i + 1 < text.length()) {
                    flush();
                    char const font = text[++i];
                    bold = font == 'B';
                    underline = font == 'I';
                } else if (e == '-') {
                    word.append('-');
                } else if (e == '&' || e == '%') {
                    continue;
                } else if (e == 'e') {
                    word.append('\\');
                } else {
                    word.append(e);
                }
                continue;
            }
            if (c == ' ' || c == '\t') {
                flush();
                continue;
            }
            word.append(c);
        }
        flush();
    }

    // A macro's arguments, with quotes keeping words together, in turn each font: .BI, .IR and so on.
    void add_alternating(StringView arguments, bool first_bold, bool first_italic, bool second_bold, bool second_italic)
    {
        size_t i = 0;
        bool second = false;
        StringBuilder joined;
        while (i < arguments.length()) {
            while (i < arguments.length() && arguments[i] == ' ')
                ++i;
            if (i >= arguments.length())
                break;
            size_t start = i;
            size_t end;
            if (arguments[i] == '"') {
                start = ++i;
                while (i < arguments.length() && arguments[i] != '"')
                    ++i;
                end = i;
                if (i < arguments.length())
                    ++i;
            } else {
                while (i < arguments.length() && arguments[i] != ' ')
                    ++i;
                end = i;
            }
            bool const bold = second ? second_bold : first_bold;
            bool const italic = second ? second_italic : first_italic;
            if (tty && bold)
                joined.append("\x1b[1m"_sv);
            if (tty && italic)
                joined.append("\x1b[4m"_sv);
            joined.append(arguments.substring_view(start, end - start));
            if (tty && (bold || italic))
                joined.append("\x1b[0m"_sv);
            second = !second;
        }
        add_word(joined.view(), false, false);
    }

    void run(StringView source)
    {
        size_t start = 0;
        while (start < source.length()) {
            auto const newline_at = source.find('\n', start);
            size_t const end = newline_at.has_value() ? newline_at.value() : source.length();
            auto const line = source.substring_view(start, end - start);
            start = end + 1;
            handle_line(line);
        }
        if (column != 0)
            newline();
    }

    void handle_line(StringView line)
    {
        if (line.is_empty()) {
            if (filling) {
                blank_line();
            } else {
                newline();
            }
            return;
        }
        if (!line.starts_with('.') && !line.starts_with('\'')) {
            if (!filling) {
                out.append(line);
                newline();
                return;
            }
            add_text(line, false, false);
            return;
        }
        auto const space = line.find(' ');
        auto const macro = line.substring_view(1, (space.has_value() ? space.value() : line.length()) - 1);
        auto const arguments = space.has_value() ? line.substring_view(space.value() + 1).trim_whitespace() : StringView {};
        if (macro == "\\\""_sv) {
            return;
        }
        if (macro == "TH"_sv) {
            // The name and section, on the top line.
            auto const name_end = arguments.find(' ');
            auto const name = name_end.has_value() ? arguments.substring_view(0, name_end.value()) : arguments;
            auto rest = name_end.has_value() ? arguments.substring_view(name_end.value() + 1) : StringView {};
            auto const section_end = rest.find(' ');
            auto const section = section_end.has_value() ? rest.substring_view(0, section_end.value()) : rest;
            StringBuilder title;
            title.append(name);
            title.append('(');
            title.append(section);
            title.append(')');
            out.append(title.view());
            int const padding = width - static_cast<int>(title.view().length()) * 2;
            for (int i = 0; i < padding; ++i)
                out.append(' ');
            out.append(title.view());
            newline();
            newline();
            return;
        }
        if (macro == "SH"_sv || macro == "SS"_sv) {
            blank_line();
            int const previous = indent;
            indent = macro == "SH"_sv ? 0 : 3;
            StringView heading = arguments;
            if (heading.starts_with('"') && heading.ends_with('"') && heading.length() >= 2)
                heading = heading.substring_view(1, heading.length() - 2);
            start_line_if_needed();
            if (tty)
                out.append("\x1b[1m"_sv);
            out.append(heading);
            if (tty)
                out.append("\x1b[0m"_sv);
            newline();
            indent = 7;
            (void)previous;
            return;
        }
        if (macro == "PP"_sv || macro == "LP"_sv || macro == "P"_sv) {
            blank_line();
            indent = 7;
            return;
        }
        if (macro == "TP"_sv) {
            blank_line();
            indent = 7;
            m_after_tag_indent = 14;
            m_next_is_tag = true;
            return;
        }
        if (macro == "IP"_sv) {
            blank_line();
            indent = 12;
            if (!arguments.is_empty()) {
                pending_indent = 7;
                add_text(arguments, false, false);
            }
            return;
        }
        if (macro == "RS"_sv) {
            indent += 7;
            return;
        }
        if (macro == "RE"_sv) {
            indent -= 7;
            return;
        }
        if (macro == "br"_sv) {
            if (column != 0)
                newline();
            return;
        }
        if (macro == "sp"_sv) {
            blank_line();
            return;
        }
        if (macro == "nf"_sv) {
            if (column != 0)
                newline();
            filling = false;
            return;
        }
        if (macro == "fi"_sv) {
            filling = true;
            return;
        }
        if (macro == "B"_sv || macro == "I"_sv) {
            add_text(arguments, macro == "B"_sv, macro == "I"_sv);
        } else if (macro == "BI"_sv) {
            add_alternating(arguments, true, false, false, true);
        } else if (macro == "IB"_sv) {
            add_alternating(arguments, false, true, true, false);
        } else if (macro == "BR"_sv) {
            add_alternating(arguments, true, false, false, false);
        } else if (macro == "RB"_sv) {
            add_alternating(arguments, false, false, true, false);
        } else if (macro == "IR"_sv) {
            add_alternating(arguments, false, true, false, false);
        } else if (macro == "RI"_sv) {
            add_alternating(arguments, false, false, false, true);
        }
        if (m_next_is_tag) {
            m_next_is_tag = false;
            indent = m_after_tag_indent;
            newline();
        }
    }

    bool m_next_is_tag { false };
    int m_after_tag_indent { 14 };
};

int find_page(char const* section, char const* name, char* out, size_t size)
{
    char const* man_path = getenv("MANPATH");
    char paths[1024];
    snprintf(paths, sizeof(paths), "%s", man_path && *man_path ? man_path : "/usr/share/man:/usr/local/share/man");
    char const* const sections[] = { "1", "8", "2", "3", "4", "5", "6", "7" };
    for (char* path = strtok(paths, ":"); path; path = strtok(nullptr, ":")) {
        for (char const* candidate : sections) {
            if (section && strcmp(section, candidate) != 0)
                continue;
            snprintf(out, size, "%s/man%s/%s.%s", path, candidate, name, candidate);
            if (access(out, R_OK) == 0)
                return 1;
        }
    }
    return 0;
}

}

int main(int argc, char** argv)
{
    bool only_where = false;
    int first = 1;
    if (first < argc && strcmp(argv[first], "-w") == 0) {
        only_where = true;
        ++first;
    }
    char const* section = nullptr;
    if (argc - first == 2) {
        section = argv[first++];
    }
    if (argc - first != 1) {
        fprintf(stderr, "What manual page do you want?\n");
        return 1;
    }
    char const* name = argv[first];
    char path[1024];
    if (!find_page(section, name, path, sizeof(path))) {
        if (section)
            fprintf(stderr, "No entry for %s in section %s of the manual\n", name, section);
        else
            fprintf(stderr, "No manual entry for %s\n", name);
        return 1;
    }
    if (only_where) {
        puts(path);
        return 0;
    }
    auto contents = read_whole_file(path);
    if (contents.is_error()) {
        fprintf(stderr, "man: %s: %s\n", path, strerror(contents.error().code()));
        return 1;
    }
    Formatter formatter;
    bool const tty = isatty(STDOUT_FILENO);
    formatter.tty = tty;
    if (tty) {
        winsize size {};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 20)
            formatter.width = size.ws_col - 2;
    }
    formatter.run(StringView(contents.value().span()));
    auto const text = formatter.out.view();
    if (tty) {
        char const* pager = getenv("MANPAGER");
        if (!pager)
            pager = getenv("PAGER");
        FILE* out = popen(pager ? pager : "more", "w");
        if (out) {
            fwrite(text.characters_without_null_termination(), 1, text.length(), out);
            pclose(out);
            return 0;
        }
    }
    fwrite(text.characters_without_null_termination(), 1, text.length(), stdout);
    return 0;
}
