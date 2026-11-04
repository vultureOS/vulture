/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TreeHelpers.h"
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>

// vi [file]: the visual editor. Moving (h j k l w b e 0 ^ $ G gg, the arrows, counts), inserting (i a I A o O), changing (x X r
// s D C dd dw d$ cc cw yy yw p P J), undo, search (/ ? n N), and ex commands (:w :q :wq :x :q! :e :N :s/// :%s/// :set nu).

namespace {

struct Editor {
    Vector<String> lines;
    char path[1024] {};
    size_t row { 0 };
    size_t column { 0 };
    size_t top { 0 };
    size_t left { 0 };
    int screen_rows { 24 };
    int screen_columns { 80 };
    bool dirty { false };
    bool show_numbers { false };
    bool running { true };
    char message[256] {};
    char search[256] {};
    bool search_forward { true };
    Vector<String> yanked;
    bool yanked_whole_lines { false };
    String undo_row_snapshot;
    // The whole text, before the last change.
    Vector<String> undo_lines;
    size_t undo_row { 0 };
    size_t undo_column { 0 };
    bool has_undo { false };
};

Editor g;
termios g_original_terminal;

String copy_string(StringView text)
{
    return MUST(String::from_view(text));
}

void restore_terminal()
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_original_terminal);
    fputs("\x1b[?1049l", stdout);
    fflush(stdout);
}

void raw_terminal()
{
    tcgetattr(STDIN_FILENO, &g_original_terminal);
    termios raw = g_original_terminal;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | ISIG | IEXTEN));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    fputs("\x1b[?1049h", stdout);
}

void find_screen_size()
{
    winsize size {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0) {
        g.screen_rows = size.ws_row;
        g.screen_columns = size.ws_col;
    }
}

void say(char const* format, char const* detail = "")
{
    snprintf(g.message, sizeof(g.message), format, detail);
}

void load(char const* path)
{
    g.lines.clear();
    snprintf(g.path, sizeof(g.path), "%s", path);
    auto contents = read_whole_file(path);
    size_t bytes = 0;
    if (!contents.is_error()) {
        auto const text = StringView(contents.value().span());
        bytes = text.length();
        size_t start = 0;
        while (start < text.length()) {
            auto const newline = text.find('\n', start);
            size_t const end = newline.has_value() ? newline.value() : text.length();
            g.lines.append(copy_string(text.substring_view(start, end - start)));
            start = end + 1;
        }
    }
    bool const is_new = contents.is_error();
    if (g.lines.is_empty())
        g.lines.append(String {});
    g.row = g.column = g.top = g.left = 0;
    g.dirty = false;
    g.has_undo = false;
    if (is_new) {
        snprintf(g.message, sizeof(g.message), "\"%s\" [New File]", path);
    } else {
        snprintf(g.message, sizeof(g.message), "\"%s\" %zu lines, %zu characters", path, g.lines.size(), bytes);
    }
}

bool save(char const* path)
{
    StringBuilder text;
    for (auto const& line : g.lines) {
        text.append(line.view());
        text.append('\n');
    }
    if (!write_whole_file(path, text.view().characters_without_null_termination(), text.view().length(), 0644)) {
        snprintf(g.message, sizeof(g.message), "\"%s\" %s", path, strerror(errno));
        return false;
    }
    snprintf(g.message, sizeof(g.message), "\"%s\" %zu lines, %zu characters written", path, g.lines.size(), text.view().length());
    g.dirty = false;
    return true;
}

void remember_for_undo()
{
    g.undo_lines.clear();
    for (auto const& line : g.lines)
        g.undo_lines.append(copy_string(line.view()));
    g.undo_row = g.row;
    g.undo_column = g.column;
    g.has_undo = true;
    g.dirty = true;
}

void undo()
{
    if (!g.has_undo) {
        say("Already at oldest change");
        return;
    }
    Vector<String> current;
    for (auto const& line : g.lines)
        current.append(copy_string(line.view()));
    g.lines = move(g.undo_lines);
    g.undo_lines = move(current);
    size_t const row = g.row, column = g.column;
    g.row = g.undo_row;
    g.column = g.undo_column;
    g.undo_row = row;
    g.undo_column = column;
    g.dirty = true;
}

void clamp_cursor(bool in_insert_mode = false)
{
    if (g.row >= g.lines.size())
        g.row = g.lines.size() - 1;
    size_t const length = g.lines[g.row].view().length();
    size_t const limit = in_insert_mode ? length : (length ? length - 1 : 0);
    if (g.column > limit)
        g.column = limit;
}

void redraw()
{
    find_screen_size();
    int const text_rows = g.screen_rows - 1;
    if (g.row < g.top)
        g.top = g.row;
    if (g.row >= g.top + static_cast<size_t>(text_rows))
        g.top = g.row - static_cast<size_t>(text_rows) + 1;
    int const number_width = g.show_numbers ? 8 : 0;
    int const text_columns = g.screen_columns - number_width;
    if (g.column < g.left)
        g.left = g.column;
    if (g.column >= g.left + static_cast<size_t>(text_columns))
        g.left = g.column - static_cast<size_t>(text_columns) + 1;
    fputs("\x1b[?25l\x1b[H", stdout);
    for (int screen = 0; screen < text_rows; ++screen) {
        size_t const index = g.top + static_cast<size_t>(screen);
        if (index >= g.lines.size()) {
            fputs("~\x1b[K\r\n", stdout);
            continue;
        }
        if (g.show_numbers)
            printf("%6zu  ", index + 1);
        auto const line = g.lines[index].view();
        if (g.left < line.length()) {
            size_t const shown = min(line.length() - g.left, static_cast<size_t>(text_columns));
            fwrite(line.characters_without_null_termination() + g.left, 1, shown, stdout);
        }
        fputs("\x1b[K\r\n", stdout);
    }
    fputs(g.message, stdout);
    fputs("\x1b[K", stdout);
    printf("\x1b[%zu;%zuH\x1b[?25h", g.row - g.top + 1, g.column - g.left + static_cast<size_t>(number_width) + 1);
    fflush(stdout);
}

enum Key {
    KeyUp = 1000,
    KeyDown,
    KeyLeft,
    KeyRight,
    KeyEnd,
};

int read_key()
{
    char c = 0;
    while (read(STDIN_FILENO, &c, 1) != 1) {
        if (errno != EINTR) {
            restore_terminal();
            exit(1);
        }
    }
    if (c != 0x1b)
        return static_cast<unsigned char>(c);
    // An arrow, if more follows at once. Otherwise, Escape.
    pollfd wait_for { STDIN_FILENO, POLLIN, 0 };
    if (poll(&wait_for, 1, 30) <= 0)
        return 0x1b;
    char sequence[2];
    if (read(STDIN_FILENO, &sequence[0], 1) != 1 || sequence[0] != '[')
        return 0x1b;
    if (read(STDIN_FILENO, &sequence[1], 1) != 1)
        return 0x1b;
    switch (sequence[1]) {
    case 'A':
        return KeyUp;
    case 'B':
        return KeyDown;
    case 'C':
        return KeyRight;
    case 'D':
        return KeyLeft;
    default:
        return 0x1b;
    }
}

bool is_word_character(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

void word_forward()
{
    auto line = g.lines[g.row].view();
    if (g.column < line.length()) {
        bool const in_word = is_word_character(line[g.column]);
        if (!is_blank(line[g.column])) {
            while (g.column < line.length() && !is_blank(line[g.column]) && is_word_character(line[g.column]) == in_word)
                ++g.column;
        }
    }
    for (;;) {
        line = g.lines[g.row].view();
        while (g.column < line.length() && is_blank(line[g.column]))
            ++g.column;
        if (g.column < line.length() || g.row + 1 >= g.lines.size())
            break;
        ++g.row;
        g.column = 0;
        if (g.lines[g.row].view().is_empty())
            break;
    }
}

void word_backward()
{
    for (;;) {
        if (g.column == 0) {
            if (g.row == 0)
                return;
            --g.row;
            g.column = g.lines[g.row].view().length();
            if (g.column == 0)
                return;
        }
        auto const line = g.lines[g.row].view();
        --g.column;
        if (g.column < line.length() && is_blank(line[g.column]))
            continue;
        bool const in_word = is_word_character(line[g.column]);
        while (g.column > 0 && !is_blank(line[g.column - 1]) && is_word_character(line[g.column - 1]) == in_word)
            --g.column;
        return;
    }
}

void word_end()
{
    if (g.column + 1 < g.lines[g.row].view().length())
        ++g.column;
    else if (g.row + 1 < g.lines.size()) {
        ++g.row;
        g.column = 0;
    }
    auto line = g.lines[g.row].view();
    while (g.column < line.length() && is_blank(line[g.column]))
        ++g.column;
    if (g.column >= line.length())
        return;
    bool const in_word = is_word_character(line[g.column]);
    while (g.column + 1 < line.length() && !is_blank(line[g.column + 1]) && is_word_character(line[g.column + 1]) == in_word)
        ++g.column;
}

void first_non_blank()
{
    auto const line = g.lines[g.row].view();
    g.column = 0;
    while (g.column < line.length() && is_blank(line[g.column]))
        ++g.column;
}

void set_line(size_t row, StringView text)
{
    g.lines[row] = copy_string(text);
}

void insert_mode()
{
    say("-- INSERT --");
    for (;;) {
        redraw();
        int const key = read_key();
        if (key == 0x1b)
            break;
        auto const line = g.lines[g.row].view();
        if (key == KeyLeft) {
            if (g.column > 0)
                --g.column;
        } else if (key == KeyRight) {
            if (g.column < line.length())
                ++g.column;
        } else if (key == KeyUp) {
            if (g.row > 0)
                --g.row;
            clamp_cursor(true);
        } else if (key == KeyDown) {
            if (g.row + 1 < g.lines.size())
                ++g.row;
            clamp_cursor(true);
        } else if (key == 127 || key == 8) {
            if (g.column > 0) {
                StringBuilder text;
                text.append(line.substring_view(0, g.column - 1));
                text.append(line.substring_view(g.column));
                set_line(g.row, text.view());
                --g.column;
            } else if (g.row > 0) {
                size_t const length = g.lines[g.row - 1].view().length();
                StringBuilder text;
                text.append(g.lines[g.row - 1].view());
                text.append(line);
                set_line(g.row - 1, text.view());
                g.lines.remove(g.row);
                --g.row;
                g.column = length;
            }
        } else if (key == '\r' || key == '\n') {
            auto const rest = copy_string(line.substring_view(g.column));
            set_line(g.row, line.substring_view(0, g.column));
            // Vector has no insert in the middle of what it holds: rebuild from here.
            g.lines.insert(g.row + 1, copy_string(rest.view()));
            ++g.row;
            g.column = 0;
        } else if (key == '\t' || (key >= 32 && key < 256)) {
            StringBuilder text;
            text.append(line.substring_view(0, g.column));
            text.append(static_cast<char>(key));
            text.append(line.substring_view(g.column));
            set_line(g.row, text.view());
            ++g.column;
        }
    }
    say("");
    if (g.column > 0)
        --g.column;
    clamp_cursor();
}

void yank_lines(size_t from, size_t count)
{
    g.yanked.clear();
    g.yanked_whole_lines = true;
    for (size_t i = from; i < from + count && i < g.lines.size(); ++i)
        g.yanked.append(copy_string(g.lines[i].view()));
}

void yank_text(StringView text)
{
    g.yanked.clear();
    g.yanked_whole_lines = false;
    g.yanked.append(copy_string(text));
}

void delete_lines(size_t from, size_t count)
{
    yank_lines(from, count);
    for (size_t i = 0; i < count && from < g.lines.size(); ++i)
        g.lines.remove(from);
    if (g.lines.is_empty())
        g.lines.append(String {});
    g.row = min(from, g.lines.size() - 1);
    first_non_blank();
}

// Removes from `from` to `to` on this line, keeping it for pasting.
void delete_range(size_t from, size_t to)
{
    auto const line = g.lines[g.row].view();
    to = min(to, line.length());
    if (from >= to)
        return;
    yank_text(line.substring_view(from, to - from));
    StringBuilder text;
    text.append(line.substring_view(0, from));
    text.append(line.substring_view(to));
    set_line(g.row, text.view());
    g.column = from;
}

void paste(bool after)
{
    if (g.yanked.is_empty())
        return;
    remember_for_undo();
    if (g.yanked_whole_lines) {
        size_t const at = after ? g.row + 1 : g.row;
        for (size_t i = 0; i < g.yanked.size(); ++i)
            g.lines.insert(at + i, copy_string(g.yanked[i].view()));
        g.row = at;
        first_non_blank();
        return;
    }
    auto const line = g.lines[g.row].view();
    size_t const at = line.is_empty() ? 0 : (after ? g.column + 1 : g.column);
    StringBuilder text;
    text.append(line.substring_view(0, at));
    text.append(g.yanked[0].view());
    text.append(line.substring_view(at));
    size_t const length = g.yanked[0].view().length();
    set_line(g.row, text.view());
    g.column = at + (length ? length - 1 : 0);
}

bool find_next(bool forward)
{
    if (!g.search[0]) {
        say("No previous regular expression");
        return false;
    }
    regex_t expression;
    if (regcomp(&expression, g.search, 0) != 0) {
        say("Bad regular expression: %s", g.search);
        return false;
    }
    size_t const total = g.lines.size();
    bool found = false;
    size_t row = g.row;
    size_t column = g.column;
    for (size_t step = 0; step <= total && !found; ++step) {
        auto const owned = copy_string(g.lines[row].view());
        regmatch_t match[1];
        char const* text = owned.characters();
        if (forward) {
            size_t const start = step == 0 ? column + 1 : 0;
            if (start <= owned.view().length() && regexec(&expression, text + start, 1, match, start ? REG_NOTBOL : 0) == 0) {
                g.row = row;
                g.column = start + static_cast<size_t>(match[0].rm_so);
                found = true;
                break;
            }
            row = row + 1 == total ? 0 : row + 1;
        } else {
            // The last match before where it starts.
            long best = -1;
            size_t at = 0;
            while (at <= owned.view().length() && regexec(&expression, text + at, 1, match, at ? REG_NOTBOL : 0) == 0) {
                size_t const position = at + static_cast<size_t>(match[0].rm_so);
                if (step == 0 && position >= column)
                    break;
                best = static_cast<long>(position);
                at = position + 1;
            }
            if (best >= 0) {
                g.row = row;
                g.column = static_cast<size_t>(best);
                found = true;
                break;
            }
            row = row == 0 ? total - 1 : row - 1;
        }
    }
    regfree(&expression);
    if (!found)
        say("Pattern not found: %s", g.search);
    return found;
}

// What's typed at the bottom of the screen, starting with this prompt.
bool read_command_line(char prompt, char* out, size_t size)
{
    size_t length = 0;
    for (;;) {
        printf("\x1b[%d;1H%c%.*s\x1b[K", g.screen_rows, prompt, static_cast<int>(length), out);
        fflush(stdout);
        int const key = read_key();
        if (key == '\r' || key == '\n') {
            out[length] = 0;
            return true;
        }
        if (key == 0x1b) {
            say("");
            return false;
        }
        if (key == 127 || key == 8) {
            if (length == 0) {
                say("");
                return false;
            }
            --length;
            continue;
        }
        if (key >= 32 && key < 256 && length + 1 < size)
            out[length++] = static_cast<char>(key);
    }
}

// :s/from/to/g on these lines.
void substitute(size_t first, size_t last, char const* command)
{
    char delimiter = command[0];
    if (!delimiter)
        return;
    char from[256], to[256];
    size_t i = 1, length = 0;
    while (command[i] && command[i] != delimiter && length + 1 < sizeof(from)) {
        if (command[i] == '\\' && command[i + 1] == delimiter)
            ++i;
        from[length++] = command[i++];
    }
    from[length] = 0;
    if (command[i] != delimiter) {
        say("Usage: s/pattern/replacement/[g]");
        return;
    }
    ++i;
    length = 0;
    while (command[i] && command[i] != delimiter && length + 1 < sizeof(to)) {
        if (command[i] == '\\' && command[i + 1] == delimiter)
            ++i;
        to[length++] = command[i++];
    }
    to[length] = 0;
    bool const every = command[i] == delimiter && command[i + 1] == 'g';
    regex_t expression;
    if (regcomp(&expression, from, 0) != 0) {
        say("Bad regular expression: %s", from);
        return;
    }
    remember_for_undo();
    size_t changed = 0;
    for (size_t row = first; row <= last && row < g.lines.size(); ++row) {
        auto const owned = copy_string(g.lines[row].view());
        char const* at = owned.characters();
        StringBuilder text;
        bool any = false;
        regmatch_t match[1];
        while (regexec(&expression, at, 1, match, at == owned.characters() ? 0 : REG_NOTBOL) == 0) {
            any = true;
            text.append(StringView(at, static_cast<size_t>(match[0].rm_so)));
            for (char const* c = to; *c; ++c) {
                if (*c == '&')
                    text.append(StringView(at + match[0].rm_so, static_cast<size_t>(match[0].rm_eo - match[0].rm_so)));
                else if (*c == '\\' && c[1])
                    text.append(*++c);
                else
                    text.append(*c);
            }
            if (match[0].rm_eo == match[0].rm_so) {
                if (at[match[0].rm_eo] == 0) {
                    at += match[0].rm_eo;
                    break;
                }
                text.append(at[match[0].rm_eo]);
                at += match[0].rm_eo + 1;
            } else {
                at += match[0].rm_eo;
            }
            if (!every)
                break;
        }
        if (!any)
            continue;
        text.append(StringView(at));
        set_line(row, text.view());
        ++changed;
    }
    regfree(&expression);
    if (changed == 0)
        say("Pattern not found: %s", from);
    clamp_cursor();
}

void run_ex_command(char* command)
{
    while (*command == ' ')
        ++command;
    // A line number on its own.
    if (*command >= '0' && *command <= '9') {
        char* rest = nullptr;
        long const line = strtol(command, &rest, 10);
        if (!*rest) {
            g.row = line <= 0 ? 0 : static_cast<size_t>(line) - 1;
            clamp_cursor();
            first_non_blank();
            return;
        }
    }
    if (*command == '$' && !command[1]) {
        g.row = g.lines.size() - 1;
        first_non_blank();
        return;
    }
    if (strncmp(command, "%s", 2) == 0) {
        substitute(0, g.lines.size() - 1, command + 2);
        return;
    }
    if (command[0] == 's' && command[1] && !((command[1] >= 'a' && command[1] <= 'z') || command[1] == ' ')) {
        substitute(g.row, g.row, command + 1);
        return;
    }
    char word[32];
    size_t used = 0;
    while (*command && *command != ' ' && used + 1 < sizeof(word))
        word[used++] = *command++;
    word[used] = 0;
    while (*command == ' ')
        ++command;
    char const* argument = command;
    if (strcmp(word, "w") == 0 || strcmp(word, "wq") == 0 || strcmp(word, "x") == 0 || strcmp(word, "w!") == 0) {
        char const* target = *argument ? argument : g.path;
        if (!*target) {
            say("No current filename");
            return;
        }
        if (!save(target))
            return;
        if (!g.path[0])
            snprintf(g.path, sizeof(g.path), "%s", target);
        if (word[1] == 'q' || word[0] == 'x')
            g.running = false;
        return;
    }
    if (strcmp(word, "q") == 0) {
        if (g.dirty) {
            say("No write since last change (:q! overrides)");
            return;
        }
        g.running = false;
        return;
    }
    if (strcmp(word, "q!") == 0) {
        g.running = false;
        return;
    }
    if (strcmp(word, "e") == 0 || strcmp(word, "e!") == 0) {
        if (g.dirty && word[1] != '!') {
            say("No write since last change (:e! overrides)");
            return;
        }
        char const* target = *argument ? argument : g.path;
        if (!*target) {
            say("No current filename");
            return;
        }
        char copy[1024];
        snprintf(copy, sizeof(copy), "%s", target);
        load(copy);
        return;
    }
    if (strcmp(word, "set") == 0) {
        if (strcmp(argument, "nu") == 0 || strcmp(argument, "number") == 0)
            g.show_numbers = true;
        else if (strcmp(argument, "nonu") == 0 || strcmp(argument, "nonumber") == 0)
            g.show_numbers = false;
        else
            say("Unknown option: %s", argument);
        return;
    }
    if (word[0])
        say("Not an editor command: %s", word);
}

void normal_mode_key(int key)
{
    static long count = 0;
    if ((key >= '1' && key <= '9') || (key == '0' && count > 0)) {
        count = count * 10 + (key - '0');
        return;
    }
    long const times = count > 0 ? count : 1;
    bool const had_count = count > 0;
    count = 0;
    g.message[0] = 0;
    auto repeat = [&](auto action) {
        for (long i = 0; i < times; ++i)
            action();
    };
    auto const line_length = [&] { return g.lines[g.row].view().length(); };
    switch (key) {
    case 'h':
    case KeyLeft:
    case 127:
        repeat([] {
            if (g.column > 0)
                --g.column;
        });
        break;
    case 'l':
    case ' ':
    case KeyRight:
        repeat([&] {
            if (g.column + 1 < line_length())
                ++g.column;
        });
        break;
    case 'k':
    case KeyUp:
        repeat([] {
            if (g.row > 0)
                --g.row;
        });
        break;
    case 'j':
    case KeyDown:
    case '\r':
        repeat([] {
            if (g.row + 1 < g.lines.size())
                ++g.row;
        });
        if (key == '\r')
            first_non_blank();
        break;
    case 'w':
        repeat(word_forward);
        break;
    case 'b':
        repeat(word_backward);
        break;
    case 'e':
        repeat(word_end);
        break;
    case '0':
        g.column = 0;
        break;
    case '^':
        first_non_blank();
        break;
    case '$':
        g.column = line_length() ? line_length() - 1 : 0;
        break;
    case 'G':
        g.row = had_count ? min(static_cast<size_t>(times) - 1, g.lines.size() - 1) : g.lines.size() - 1;
        first_non_blank();
        break;
    case 'g': {
        int const next = read_key();
        if (next == 'g') {
            g.row = had_count ? min(static_cast<size_t>(times) - 1, g.lines.size() - 1) : 0;
            first_non_blank();
        }
        break;
    }
    case 'i':
        remember_for_undo();
        insert_mode();
        break;
    case 'a':
        remember_for_undo();
        if (line_length())
            ++g.column;
        insert_mode();
        break;
    case 'I':
        remember_for_undo();
        first_non_blank();
        insert_mode();
        break;
    case 'A':
        remember_for_undo();
        g.column = line_length();
        insert_mode();
        break;
    case 'o':
        remember_for_undo();
        g.lines.insert(g.row + 1, String {});
        ++g.row;
        g.column = 0;
        insert_mode();
        break;
    case 'O':
        remember_for_undo();
        g.lines.insert(g.row, String {});
        g.column = 0;
        insert_mode();
        break;
    case 'x':
        if (line_length()) {
            remember_for_undo();
            delete_range(g.column, g.column + static_cast<size_t>(times));
        }
        break;
    case 'X':
        if (g.column > 0) {
            remember_for_undo();
            size_t const from = g.column > static_cast<size_t>(times) ? g.column - static_cast<size_t>(times) : 0;
            delete_range(from, g.column);
        }
        break;
    case 's':
        remember_for_undo();
        delete_range(g.column, g.column + static_cast<size_t>(times));
        insert_mode();
        break;
    case 'r': {
        int const replacement = read_key();
        if (replacement >= 32 && replacement < 256 && line_length()) {
            remember_for_undo();
            StringBuilder text;
            auto const line = g.lines[g.row].view();
            text.append(line.substring_view(0, g.column));
            text.append(static_cast<char>(replacement));
            text.append(line.substring_view(g.column + 1));
            set_line(g.row, text.view());
        }
        break;
    }
    case 'D':
        remember_for_undo();
        delete_range(g.column, line_length());
        break;
    case 'C':
        remember_for_undo();
        delete_range(g.column, line_length());
        insert_mode();
        break;
    case 'J':
        if (g.row + 1 < g.lines.size()) {
            remember_for_undo();
            size_t const join_at = line_length();
            StringBuilder text;
            text.append(g.lines[g.row].view());
            auto const next = g.lines[g.row + 1].view().trim_whitespace();
            if (join_at && !next.is_empty())
                text.append(' ');
            text.append(next);
            set_line(g.row, text.view());
            g.lines.remove(g.row + 1);
            g.column = join_at;
        }
        break;
    case 'd':
    case 'c':
    case 'y': {
        int const motion = read_key();
        if (motion == key) {
            if (key == 'y') {
                yank_lines(g.row, static_cast<size_t>(times));
                break;
            }
            remember_for_undo();
            if (key == 'c') {
                yank_lines(g.row, 1);
                set_line(g.row, {});
                g.column = 0;
                insert_mode();
            } else {
                delete_lines(g.row, static_cast<size_t>(times));
            }
            break;
        }
        size_t const start_column = g.column;
        size_t const start_row = g.row;
        if (motion == 'w' || motion == 'e') {
            if (motion == 'w' && key == 'c')
                repeat(word_end);
            else if (motion == 'w')
                repeat(word_forward);
            else
                repeat(word_end);
            if (g.row != start_row) {
                g.row = start_row;
                g.column = line_length();
            } else if (motion == 'e' || key == 'c') {
                ++g.column;
            }
        } else if (motion == '$') {
            g.column = line_length();
        } else if (motion == '0') {
            g.column = 0;
        } else if (motion == 'b') {
            repeat(word_backward);
        } else {
            break;
        }
        size_t const low = min(start_column, g.column);
        size_t const high = max(start_column, g.column);
        if (key == 'y') {
            yank_text(g.lines[g.row].view().substring_view(low, min(high, line_length()) - low));
            g.column = low;
            break;
        }
        g.column = start_column;
        remember_for_undo();
        delete_range(low, high);
        if (key == 'c')
            insert_mode();
        break;
    }
    case 'p':
        paste(true);
        break;
    case 'P':
        paste(false);
        break;
    case 'u':
        undo();
        break;
    case '/':
    case '?': {
        char pattern[256];
        if (!read_command_line(static_cast<char>(key), pattern, sizeof(pattern)))
            break;
        if (pattern[0])
            snprintf(g.search, sizeof(g.search), "%s", pattern);
        g.search_forward = key == '/';
        (void)find_next(g.search_forward);
        break;
    }
    case 'n':
        (void)find_next(g.search_forward);
        break;
    case 'N':
        (void)find_next(!g.search_forward);
        break;
    case ':': {
        char command[512];
        if (read_command_line(':', command, sizeof(command)))
            run_ex_command(command);
        break;
    }
    case 'Z': {
        if (read_key() == 'Z') {
            if (!g.path[0]) {
                say("No current filename");
                break;
            }
            if (!g.dirty || save(g.path))
                g.running = false;
        }
        break;
    }
    case 0x1b:
        break;
    default:
        break;
    }
    clamp_cursor();
}

}

int main(int argc, char** argv)
{
    if (argc > 2) {
        fprintf(stderr, "usage: vi [file]\n");
        return 1;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "vi: not a terminal\n");
        return 1;
    }
    if (argc == 2) {
        load(argv[1]);
    } else {
        g.lines.append(String {});
    }
    raw_terminal();
    while (g.running) {
        redraw();
        normal_mode_key(read_key());
    }
    restore_terminal();
    return 0;
}
