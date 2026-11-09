/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/String.h>
#include <LibDatabase/Database.h>

extern "C" {
long read(int fd, void* buffer, unsigned long count);
long write(int fd, void const* buffer, unsigned long count);
}

// dbshell DATABASE < statements: each statement's rows, and what went wrong with those that went wrong, as sqlite3 says them.

static String read_everything(int fd)
{
    StringBuilder text;
    char buffer[65536];
    long got;
    while ((got = read(fd, buffer, sizeof(buffer))) > 0)
        text.append(StringView(buffer, static_cast<size_t>(got)));
    return MUST(text.to_string());
}

static void say(StringView text)
{
    (void)write(1, text.characters_without_null_termination(), text.length());
}

// Where the statement starting here ends: the first semicolon outside quotes and comments.
static size_t end_of_statement(StringView text, size_t start)
{
    size_t i = start;
    while (i < text.length()) {
        char c = text[i];
        if (c == '\'' || c == '"') {
            ++i;
            while (i < text.length() && text[i] != c)
                ++i;
        } else if (c == '-' && i + 1 < text.length() && text[i + 1] == '-') {
            while (i < text.length() && text[i] != '\n')
                ++i;
        } else if (c == ';') {
            return i;
        }
        ++i;
    }
    return text.length();
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        say("usage: dbshell DATABASE < statements\n"_sv);
        return 2;
    }
    auto database = Database::Database::open(StringView(argv[1], __builtin_strlen(argv[1])));
    if (database.is_error()) {
        say("dbshell: can't open it\n"_sv);
        return 1;
    }
    auto text = read_everything(0);
    auto view = text.view();
    size_t position = 0;
    while (position < view.length()) {
        size_t end = end_of_statement(view, position);
        auto statement = view.substring_view(position, end - position).trim_whitespace();
        position = end + 1;
        if (statement.is_empty())
            continue;
        auto rows = database.value()->query(statement);
        if (rows.is_error()) {
            say("Error: "_sv);
            say(database.value()->last_error());
            say("\n"_sv);
            continue;
        }
        for (auto const& row : rows.value()) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i)
                    say("|"_sv);
                say(row[i].to_text().view());
            }
            say("\n"_sv);
        }
    }
    return 0;
}
