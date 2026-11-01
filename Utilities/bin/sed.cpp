/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <ctype.h>
#include <regex.h>
#include <unistd.h>

// sed [-nE] [-i[suffix]] [-e script]... [script] [file...]
//
// Addresses: N, $, /regex/, and ranges a,b (with ! to negate). Commands:
// s/regex/replacement/[gpNI] (& and \1-\9 in the replacement), d, p, q,
// =, y/abc/xyz/, a text, i text, c text, and { ... } groups; ; or newline
// between commands.
namespace {

struct Address {
    enum class Kind {
        None,
        Line,
        Last,
        Pattern,
    };
    Kind kind { Kind::None };
    long line { 0 };
    regex_t pattern {};
};

struct Command {
    Address first, second;
    bool negate { false };
    bool in_range { false };
    char name { 0 };
    // s
    regex_t pattern {};
    char* replacement { nullptr };
    bool global { false };
    bool print { false };
    int occurrence { 1 };
    // y
    char from[256] {}, to[256] {};
    // a i c
    char* text { nullptr };
    // { }: the index of the matching }
    size_t block_end { 0 };
};

Command s_commands[256];
size_t s_command_count = 0;
bool s_quiet = false, s_extended = false;
long s_line_number = 0;
bool s_last_line = false;

void die(char const* message)
{
    fprintf(stderr, "sed: %s\n", message);
    exit(1);
}

// Reads text up to an unescaped `delimiter`.
char* read_delimited(char const*& cursor, char delimiter, bool keep_escapes)
{
    char buffer[4096];
    size_t length = 0;
    while (*cursor && *cursor != delimiter && length < sizeof(buffer) - 2) {
        if (*cursor == '\\' && cursor[1]) {
            if (cursor[1] == delimiter) {
                buffer[length++] = delimiter;
                cursor += 2;
                continue;
            }
            if (cursor[1] == 'n' && !keep_escapes) {
                buffer[length++] = '\n';
                cursor += 2;
                continue;
            }
            buffer[length++] = *cursor++;
        }
        buffer[length++] = *cursor++;
    }
    if (*cursor != delimiter)
        die("unterminated regular expression or substitution");
    ++cursor;
    buffer[length] = '\0';
    return strdup(buffer);
}

void compile_pattern(regex_t& pattern, char const* text, bool ignore_case)
{
    int error = regcomp(&pattern, text, (s_extended ? REG_EXTENDED : 0) | (ignore_case ? REG_ICASE : 0));
    if (error) {
        char message[128];
        regerror(error, &pattern, message, sizeof(message));
        fprintf(stderr, "sed: %s: %s\n", text, message);
        exit(1);
    }
}

bool parse_address(char const*& cursor, Address& address)
{
    if (isdigit(static_cast<unsigned char>(*cursor))) {
        address.kind = Address::Kind::Line;
        address.line = strtol(cursor, const_cast<char**>(&cursor), 10);
        return true;
    }
    if (*cursor == '$') {
        address.kind = Address::Kind::Last;
        ++cursor;
        return true;
    }
    if (*cursor == '/' || *cursor == '\\') {
        char delimiter = *cursor == '\\' ? cursor[1] : '/';
        cursor += *cursor == '\\' ? 2 : 1;
        char* text = read_delimited(cursor, delimiter, true);
        address.kind = Address::Kind::Pattern;
        compile_pattern(address.pattern, text, false);
        free(text);
        return true;
    }
    return false;
}

void parse(char const* script)
{
    size_t block_starts[32];
    size_t depth = 0;
    char const* cursor = script;
    for (;;) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ';'))
            ++cursor;
        if (!*cursor)
            break;
        if (s_command_count >= sizeof(s_commands) / sizeof(s_commands[0]))
            die("too many commands");
        auto& command = s_commands[s_command_count];
        if (*cursor == '}') {
            if (depth == 0)
                die("unexpected }");
            s_commands[block_starts[--depth]].block_end = s_command_count;
            command.name = '}';
            ++s_command_count;
            ++cursor;
            continue;
        }
        if (parse_address(cursor, command.first)) {
            if (*cursor == ',') {
                ++cursor;
                if (!parse_address(cursor, command.second))
                    die("expected an address after ,");
            }
        }
        while (*cursor == ' ')
            ++cursor;
        if (*cursor == '!') {
            command.negate = true;
            ++cursor;
            while (*cursor == ' ')
                ++cursor;
        }
        command.name = *cursor++;
        switch (command.name) {
        case 's': {
            char delimiter = *cursor++;
            char* pattern = read_delimited(cursor, delimiter, true);
            command.replacement = read_delimited(cursor, delimiter, false);
            bool ignore_case = false;
            while (*cursor && *cursor != ';' && *cursor != '\n' && *cursor != '}' && !isspace(static_cast<unsigned char>(*cursor))) {
                if (*cursor == 'g')
                    command.global = true;
                else if (*cursor == 'p')
                    command.print = true;
                else if (*cursor == 'I' || *cursor == 'i')
                    ignore_case = true;
                else if (isdigit(static_cast<unsigned char>(*cursor)))
                    command.occurrence = static_cast<int>(strtol(cursor, const_cast<char**>(&cursor), 10)) , --cursor;
                ++cursor;
            }
            compile_pattern(command.pattern, pattern, ignore_case);
            free(pattern);
            break;
        }
        case 'y': {
            char delimiter = *cursor++;
            char* from = read_delimited(cursor, delimiter, false);
            char* to = read_delimited(cursor, delimiter, false);
            if (strlen(from) != strlen(to))
                die("y: strings are not the same length");
            strlcpy(command.from, from, sizeof(command.from));
            strlcpy(command.to, to, sizeof(command.to));
            free(from);
            free(to);
            break;
        }
        case 'a':
        case 'i':
        case 'c': {
            if (*cursor == '\\')
                ++cursor;
            if (*cursor == '\n')
                ++cursor;
            while (*cursor == ' ')
                ++cursor;
            char const* end = cursor;
            while (*end && *end != '\n')
                ++end;
            command.text = strndup(cursor, static_cast<size_t>(end - cursor));
            cursor = end;
            break;
        }
        case '{':
            if (depth >= 32)
                die("blocks nested too deeply");
            block_starts[depth++] = s_command_count;
            break;
        case 'd':
        case 'p':
        case 'q':
        case '=':
        case 'n':
            break;
        default:
            fprintf(stderr, "sed: unknown command: '%c'\n", command.name);
            exit(1);
        }
        ++s_command_count;
    }
    if (depth != 0)
        die("unmatched {");
}

bool address_matches(Address const& address, char const* line)
{
    switch (address.kind) {
    case Address::Kind::None:
        return true;
    case Address::Kind::Line:
        return s_line_number == address.line;
    case Address::Kind::Last:
        return s_last_line;
    case Address::Kind::Pattern:
        return regexec(&address.pattern, line, 0, nullptr, 0) == 0;
    }
    return false;
}

bool selects(Command& command, char const* line)
{
    bool result;
    if (command.first.kind == Address::Kind::None) {
        result = true;
    } else if (command.second.kind == Address::Kind::None) {
        result = address_matches(command.first, line);
    } else if (command.in_range) {
        result = true;
        bool end = command.second.kind == Address::Kind::Line ? s_line_number >= command.second.line : address_matches(command.second, line);
        if (end)
            command.in_range = false;
    } else if (address_matches(command.first, line)) {
        result = true;
        bool ends_now = command.second.kind == Address::Kind::Line && command.second.line <= s_line_number;
        command.in_range = !ends_now;
    } else {
        result = false;
    }
    return result != command.negate;
}

// s///: the new line, or nothing if no substitution happened.
char* substitute(Command const& command, char const* line)
{
    size_t capacity = strlen(line) * 2 + 256, length = 0;
    auto* output = static_cast<char*>(malloc(capacity));
    auto append = [&](char const* text, size_t size) {
        if (length + size + 1 > capacity) {
            capacity = (length + size + 1) * 2;
            output = static_cast<char*>(realloc(output, capacity));
        }
        memcpy(output + length, text, size);
        length += size;
    };
    char const* cursor = line;
    int occurrence = 0;
    bool changed = false;
    regmatch_t matches[10];
    while (*cursor || cursor == line) {
        if (regexec(&command.pattern, cursor, 10, matches, cursor > line ? REG_NOTBOL : 0) != 0)
            break;
        ++occurrence;
        append(cursor, static_cast<size_t>(matches[0].rm_so));
        if (occurrence >= command.occurrence) {
            changed = true;
            for (char const* r = command.replacement; *r; ++r) {
                if (*r == '&') {
                    append(cursor + matches[0].rm_so, static_cast<size_t>(matches[0].rm_eo - matches[0].rm_so));
                } else if (*r == '\\' && r[1] >= '0' && r[1] <= '9') {
                    int group = *++r - '0';
                    if (matches[group].rm_so >= 0)
                        append(cursor + matches[group].rm_so, static_cast<size_t>(matches[group].rm_eo - matches[group].rm_so));
                } else if (*r == '\\' && r[1]) {
                    ++r;
                    append(*r == 'n' ? "\n" : r, 1);
                } else {
                    append(r, 1);
                }
            }
        } else {
            append(cursor + matches[0].rm_so, static_cast<size_t>(matches[0].rm_eo - matches[0].rm_so));
        }
        cursor += matches[0].rm_eo;
        if (matches[0].rm_eo == matches[0].rm_so) {
            if (!*cursor)
                break;
            append(cursor, 1);
            ++cursor;
        }
        if (changed && !command.global)
            break;
    }
    if (!changed) {
        free(output);
        return nullptr;
    }
    append(cursor, strlen(cursor));
    output[length] = '\0';
    return output;
}

// Runs the script on one line; false to stop reading (q).
bool run(char*& line, FILE* out, FILE* in, bool& printed_already, char*& next_line)
{
    (void)in;
    (void)next_line;
    bool deleted = false, quit = false;
    char* appended[64];
    size_t appended_count = 0;
    for (size_t i = 0; i < s_command_count && !deleted && !quit; ++i) {
        auto& command = s_commands[i];
        if (command.name == '}')
            continue;
        if (!selects(command, line)) {
            if (command.name == '{')
                i = command.block_end;
            continue;
        }
        switch (command.name) {
        case 's':
            if (char* result = substitute(command, line)) {
                free(line);
                line = result;
                if (command.print)
                    fprintf(out, "%s\n", line);
            }
            break;
        case 'y':
            for (char* c = line; *c; ++c) {
                if (char const* found = strchr(command.from, *c))
                    *c = command.to[found - command.from];
            }
            break;
        case 'd':
            deleted = true;
            break;
        case 'p':
            fprintf(out, "%s\n", line);
            break;
        case 'q':
            quit = true;
            break;
        case '=':
            fprintf(out, "%ld\n", s_line_number);
            break;
        case 'a':
            if (appended_count < 64)
                appended[appended_count++] = command.text;
            break;
        case 'i':
            fprintf(out, "%s\n", command.text);
            break;
        case 'c':
            if (command.second.kind == Address::Kind::None || !command.in_range)
                fprintf(out, "%s\n", command.text);
            deleted = true;
            break;
        default:
            break;
        }
    }
    if (!deleted && !s_quiet)
        fprintf(out, "%s\n", line);
    for (size_t i = 0; i < appended_count; ++i)
        fprintf(out, "%s\n", appended[i]);
    printed_already = true;
    return !quit;
}

bool process(FILE* in, FILE* out, bool last_file)
{
    char* line = nullptr;
    size_t size = 0;
    ssize_t length = getline(&line, &size, in);
    while (length >= 0) {
        if (length > 0 && line[length - 1] == '\n')
            line[--length] = '\0';
        char* next = nullptr;
        size_t next_size = 0;
        ssize_t next_length = getline(&next, &next_size, in);
        ++s_line_number;
        s_last_line = next_length < 0 && last_file;
        char* current = strdup(line);
        bool printed = false;
        bool go_on = run(current, out, in, printed, next);
        free(current);
        free(line);
        line = next;
        size = next_size;
        length = next_length;
        if (!go_on) {
            free(line);
            return false;
        }
    }
    free(line);
    return true;
}

}

int main(int argc, char** argv)
{
    bool in_place = false;
    char const* suffix = "";
    char script[8192] = {};
    size_t script_length = 0;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        char const* option = argv[first];
        if (strcmp(option, "-n") == 0) {
            s_quiet = true;
        } else if (strcmp(option, "-E") == 0 || strcmp(option, "-r") == 0) {
            s_extended = true;
        } else if (strncmp(option, "-i", 2) == 0) {
            in_place = true;
            suffix = option + 2;
        } else if (strcmp(option, "-e") == 0 && first + 1 < argc) {
            script_length += static_cast<size_t>(snprintf(script + script_length, sizeof(script) - script_length, "%s\n", argv[++first]));
        } else {
            fprintf(stderr, "usage: sed [-En] [-i[extension]] script [file ...]\n       sed [-En] [-i[extension]] [-e script] ... [file ...]\n");
            return 1;
        }
    }
    if (script_length == 0) {
        if (first >= argc) {
            fprintf(stderr, "usage: sed [-En] [-i[extension]] script [file ...]\n");
            return 1;
        }
        strlcpy(script, argv[first++], sizeof(script));
    }
    parse(script);
    if (first == argc) {
        process(stdin, stdout, true);
        return 0;
    }
    int status = 0;
    for (int i = first; i < argc; ++i) {
        FILE* in = fopen(argv[i], "r");
        if (!in) {
            warn("sed", argv[i]);
            status = 1;
            continue;
        }
        if (!in_place) {
            if (!process(in, stdout, i == argc - 1)) {
                fclose(in);
                break;
            }
            fclose(in);
            continue;
        }
        char temporary[1024];
        snprintf(temporary, sizeof(temporary), "%s.sedXXXXXX", argv[i]);
        int fd = mkstemp(temporary);
        FILE* out = fd >= 0 ? fdopen(fd, "w") : nullptr;
        if (!out) {
            warn("sed", argv[i]);
            fclose(in);
            status = 1;
            continue;
        }
        s_line_number = 0;
        process(in, out, true);
        fclose(in);
        fclose(out);
        if (*suffix) {
            char backup[1024];
            snprintf(backup, sizeof(backup), "%s%s", argv[i], suffix);
            rename(argv[i], backup);
        }
        rename(temporary, argv[i]);
    }
    fflush(stdout);
    return status;
}
