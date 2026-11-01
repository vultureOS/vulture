/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Shell.h"
#include <LibBase/CharacterTypes.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace Shell {

namespace {

String string(StringView view)
{
    return MUST(String::from_view(view));
}

bool is_name_start(char c)
{
    return Base::is_ascii_alpha(static_cast<u8>(c)) || c == '_';
}

bool is_name_character(char c)
{
    return Base::is_ascii_alphanumeric(static_cast<u8>(c)) || c == '_';
}

// The index of the `close` matching an `open` at `start`, honoring quotes;
// the length of `text` if there is none.
size_t find_closing(StringView text, size_t start, char open, char close)
{
    int depth = 0;
    for (size_t i = start; i < text.length(); ++i) {
        char c = text[i];
        if (c == '\\') {
            ++i;
            continue;
        }
        if (c == '\'' && open != '\'') {
            while (++i < text.length() && text[i] != '\'') {
            }
            continue;
        }
        if (c == '"' && open != '"') {
            while (++i < text.length() && text[i] != '"') {
                if (text[i] == '\\')
                    ++i;
            }
            continue;
        }
        if (c == open) {
            ++depth;
        } else if (c == close && --depth == 0) {
            return i;
        }
    }
    return text.length();
}

struct Field {
    StringBuilder text;
    StringBuilder pattern;
    bool has_glob { false };
    bool keep { false };
};

bool is_glob_character(char c)
{
    return c == '*' || c == '?' || c == '[';
}

// Turns one word, as written, into fields: quotes interpreted, parameters,
// command substitutions and arithmetic expanded, unquoted results split on
// IFS. Each field also gets a glob pattern in which quoted characters are
// escaped, for pathname expansion.
class Expander {
public:
    Expander(Shell& shell, bool split)
        : m_shell(shell)
        , m_split(split)
    {
    }

    bool here_document { false };
    Vector<Field> fields;

    bool expand(StringView raw)
    {
        size_t i = 0;
        bool in_double = false;
        if (!here_document && raw.starts_with('~'))
            i = expand_tilde(raw);
        while (i < raw.length() && !m_failed) {
            char c = raw[i];
            if (here_document) {
                if (c == '\\' && i + 1 < raw.length() && (raw[i + 1] == '$' || raw[i + 1] == '`' || raw[i + 1] == '\\')) {
                    literal(raw[i + 1], true);
                    i += 2;
                    continue;
                }
            } else if (!in_double) {
                if (c == '\'') {
                    size_t close = i + 1;
                    while (close < raw.length() && raw[close] != '\'')
                        ++close;
                    current().keep = true;
                    for (size_t j = i + 1; j < close; ++j)
                        literal(raw[j], true);
                    i = close + 1;
                    continue;
                }
                if (c == '"') {
                    in_double = true;
                    current().keep = true;
                    ++i;
                    continue;
                }
                if (c == '\\') {
                    if (i + 1 < raw.length())
                        literal(raw[i + 1], true);
                    i += 2;
                    continue;
                }
            } else {
                if (c == '"') {
                    in_double = false;
                    ++i;
                    continue;
                }
                if (c == '\\' && i + 1 < raw.length() && (raw[i + 1] == '$' || raw[i + 1] == '`' || raw[i + 1] == '"' || raw[i + 1] == '\\' || raw[i + 1] == '\n')) {
                    if (raw[i + 1] != '\n')
                        literal(raw[i + 1], true);
                    i += 2;
                    continue;
                }
            }
            bool quoted = in_double || here_document;
            if (c == '$') {
                i = expand_dollar(raw, i, quoted);
                continue;
            }
            if (c == '`') {
                StringBuilder inner;
                size_t j = i + 1;
                for (; j < raw.length() && raw[j] != '`'; ++j) {
                    if (raw[j] == '\\' && j + 1 < raw.length() && (raw[j + 1] == '`' || raw[j + 1] == '\\' || raw[j + 1] == '$'))
                        ++j;
                    inner.append(raw[j]);
                }
                expanded(m_shell.command_substitution(inner.view()).view(), quoted);
                i = j + 1;
                continue;
            }
            literal(c, quoted);
            ++i;
        }
        return !m_failed;
    }

private:
    Field& current()
    {
        if (fields.is_empty() || m_new_field_pending) {
            fields.append(Field {});
            m_new_field_pending = false;
        }
        return fields.last();
    }

    void literal(char c, bool quoted)
    {
        auto& field = current();
        field.text.append(c);
        field.keep = true;
        if (is_glob_character(c) || c == '\\' || c == ']') {
            if (quoted || c == '\\')
                field.pattern.append('\\');
            else if (c != ']')
                field.has_glob = true;
        }
        field.pattern.append(c);
    }

    // An expansion's result: quoted, it is literal text; unquoted, it is
    // split into fields on IFS (and may be globbed).
    void expanded(StringView value, bool quoted)
    {
        if (quoted || !m_split) {
            current().keep = true;
            for (char c : value)
                literal(c, quoted);
            return;
        }
        auto ifs = m_shell.variable("IFS"_sv);
        StringView separators = ifs.has_value() ? ifs.value() : " \t\n"_sv;
        for (char c : value) {
            if (separators.contains(c)) {
                if (!fields.is_empty() && fields.last().keep)
                    m_new_field_pending = true;
                continue;
            }
            literal(c, false);
        }
    }

    size_t expand_tilde(StringView raw)
    {
        size_t end = 1;
        while (end < raw.length() && raw[end] != '/')
            ++end;
        StringView name = raw.substring_view(1, end - 1);
        for (char c : name) {
            if (!is_name_character(c) && c != '-' && c != '.')
                return 0;
        }
        char const* home = nullptr;
        if (name.is_empty()) {
            auto value = m_shell.variable("HOME"_sv);
            if (value.has_value())
                return expanded(value.value(), true), end;
            if (struct passwd* user = getpwuid(getuid()))
                home = user->pw_dir;
        } else {
            char buffer[64];
            if (name.length() < sizeof(buffer)) {
                memcpy(buffer, name.characters_without_null_termination(), name.length());
                buffer[name.length()] = '\0';
                if (struct passwd* user = getpwnam(buffer))
                    home = user->pw_dir;
            }
        }
        if (!home)
            return 0;
        expanded(StringView(home), true);
        return end;
    }

    StringView special_value(char name, StringBuilder& storage)
    {
        switch (name) {
        case '?':
            storage.appendff("{}", m_shell.last_status());
            return storage.view();
        case '$':
            storage.appendff("{}", m_shell.pid());
            return storage.view();
        case '#':
            storage.appendff("{}", m_shell.positional().size());
            return storage.view();
        case '!': {
            auto last = m_shell.variable("!"_sv);
            return last.has_value() ? last.value() : ""_sv;
        }
        case '-':
            if (m_shell.is_interactive())
                storage.append('i');
            if (m_shell.errexit)
                storage.append('e');
            if (m_shell.xtrace)
                storage.append('x');
            return storage.view();
        default:
            return {};
        }
    }

    Optional<StringView> lookup(StringView name, StringBuilder& storage)
    {
        if (name.length() == 1 && Base::is_ascii_digit(static_cast<u8>(name[0]))) {
            size_t index = static_cast<size_t>(name[0] - '0');
            if (index == 0) {
                auto zero = m_shell.variable("0"_sv);
                return zero.has_value() ? zero.value() : "vsh"_sv;
            }
            if (index <= m_shell.positional().size())
                return m_shell.positional()[index - 1].view();
            return {};
        }
        if (name.length() == 1 && (name[0] == '?' || name[0] == '$' || name[0] == '#' || name[0] == '-'))
            return special_value(name[0], storage);
        if (name.length() == 1 && (name[0] == '@' || name[0] == '*')) {
            for (size_t i = 0; i < m_shell.positional().size(); ++i) {
                if (i > 0)
                    storage.append(' ');
                storage.append(m_shell.positional()[i].view());
            }
            return storage.view();
        }
        if (name.length() > 1 && Base::is_ascii_digit(static_cast<u8>(name[0]))) {
            auto index = name.to_number<size_t>();
            if (index.has_value() && index.value() >= 1 && index.value() <= m_shell.positional().size())
                return m_shell.positional()[index.value() - 1].view();
            return {};
        }
        return m_shell.variable(name);
    }

    void fail(StringView name, StringView message)
    {
        fprintf(stderr, "vsh: %.*s: %.*s\n", static_cast<int>(name.length()), name.characters_without_null_termination(),
            static_cast<int>(message.length()), message.characters_without_null_termination());
        m_failed = true;
    }

    size_t expand_dollar(StringView raw, size_t i, bool quoted)
    {
        if (i + 1 >= raw.length()) {
            literal('$', quoted);
            return i + 1;
        }
        char next = raw[i + 1];
        if (next == '(' && i + 2 < raw.length() && raw[i + 2] == '(') {
            size_t close = find_closing(raw, i + 1, '(', ')');
            if (close >= raw.length() || close < i + 4 || raw[close - 1] != ')') {
                // "$((" that is really "$( (": a command substitution.
                return command_substitution(raw, i, quoted);
            }
            StringView expression = raw.substring_view(i + 3, close - 1 - (i + 3));
            String expanded_expression;
            if (!m_shell.expand_single(expression, expanded_expression)) {
                m_failed = true;
                return raw.length();
            }
            i64 value = 0;
            if (!m_shell.evaluate_arithmetic(expanded_expression.view(), value)) {
                m_failed = true;
                return raw.length();
            }
            StringBuilder text;
            text.appendff("{}", value);
            expanded(text.view(), quoted);
            return close + 1;
        }
        if (next == '(')
            return command_substitution(raw, i, quoted);
        if (next == '{') {
            size_t close = find_closing(raw, i + 1, '{', '}');
            expand_braced(raw.substring_view(i + 2, close - (i + 2)), quoted);
            return close + 1;
        }
        if (next == '@' || next == '*') {
            auto& parameters = m_shell.positional();
            if (quoted && next == '@') {
                // "$@": one field per parameter.
                for (size_t p = 0; p < parameters.size(); ++p) {
                    if (p > 0)
                        m_new_field_pending = true;
                    current().keep = true;
                    for (char c : parameters[p].view())
                        literal(c, true);
                }
                if (parameters.is_empty() && !fields.is_empty() && fields.last().text.length() == 0)
                    fields.last().keep = false;
                return i + 2;
            }
            StringBuilder joined;
            for (size_t p = 0; p < parameters.size(); ++p) {
                if (p > 0)
                    joined.append(' ');
                joined.append(parameters[p].view());
            }
            expanded(joined.view(), quoted);
            return i + 2;
        }
        if (next == '?' || next == '$' || next == '#' || next == '!' || next == '-' || Base::is_ascii_digit(static_cast<u8>(next))) {
            StringBuilder storage;
            auto value = lookup(raw.substring_view(i + 1, 1), storage);
            expanded(value.has_value() ? value.value() : ""_sv, quoted);
            return i + 2;
        }
        if (is_name_start(next)) {
            size_t end = i + 1;
            while (end < raw.length() && is_name_character(raw[end]))
                ++end;
            StringView name = raw.substring_view(i + 1, end - i - 1);
            auto value = m_shell.variable(name);
            if (!value.has_value() && m_shell.nounset) {
                fail(name, "unbound variable"_sv);
                return raw.length();
            }
            expanded(value.has_value() ? value.value() : ""_sv, quoted);
            return end;
        }
        literal('$', quoted);
        return i + 1;
    }

    size_t command_substitution(StringView raw, size_t i, bool quoted)
    {
        size_t close = find_closing(raw, i + 1, '(', ')');
        StringView inner = raw.substring_view(i + 2, min(close, raw.length()) - (i + 2));
        expanded(m_shell.command_substitution(inner).view(), quoted);
        return close + 1;
    }

    // Removes the shortest or longest prefix or suffix matching `pattern`.
    static String trim(StringView value, StringView pattern, bool suffix, bool longest)
    {
        char text[4096];
        char pattern_text[1024];
        if (value.length() >= sizeof(text) || pattern.length() >= sizeof(pattern_text))
            return string(value);
        memcpy(pattern_text, pattern.characters_without_null_termination(), pattern.length());
        pattern_text[pattern.length()] = '\0';
        size_t length = value.length();
        for (size_t step = 0; step <= length; ++step) {
            size_t cut = longest ? length - step : step;
            StringView candidate = suffix ? value.substring_view(length - cut) : value.substring_view(0, cut);
            memcpy(text, candidate.characters_without_null_termination(), candidate.length());
            text[candidate.length()] = '\0';
            if (fnmatch(pattern_text, text, 0) == 0)
                return string(suffix ? value.substring_view(0, length - cut) : value.substring_view(cut));
        }
        return string(value);
    }

    void expand_braced(StringView content, bool quoted)
    {
        StringBuilder storage;
        // ${#name}: the length.
        if (content.length() > 1 && content[0] == '#') {
            auto value = lookup(content.substring_view(1), storage);
            StringBuilder length;
            length.appendff("{}", value.has_value() ? value->length() : 0);
            expanded(length.view(), quoted);
            return;
        }
        size_t name_length = 0;
        if (!content.is_empty() && (content[0] == '?' || content[0] == '$' || content[0] == '#' || content[0] == '!' || content[0] == '-' || content[0] == '@' || content[0] == '*')) {
            name_length = 1;
        } else if (!content.is_empty() && Base::is_ascii_digit(static_cast<u8>(content[0]))) {
            while (name_length < content.length() && Base::is_ascii_digit(static_cast<u8>(content[name_length])))
                ++name_length;
        } else {
            while (name_length < content.length() && is_name_character(content[name_length]))
                ++name_length;
        }
        StringView name = content.substring_view(0, name_length);
        if (name.is_empty()) {
            fail(content, "bad substitution"_sv);
            return;
        }
        StringView rest = content.substring_view(name_length);
        auto value = lookup(name, storage);
        if (rest.is_empty()) {
            if (!value.has_value() && m_shell.nounset) {
                fail(name, "unbound variable"_sv);
                return;
            }
            expanded(value.has_value() ? value.value() : ""_sv, quoted);
            return;
        }
        bool colon = rest[0] == ':';
        StringView operation = colon ? rest.substring_view(1) : rest;
        if (operation.is_empty()) {
            fail(content, "bad substitution"_sv);
            return;
        }
        char op = operation[0];
        StringView word = operation.substring_view(1);
        bool unset_or_empty = !value.has_value() || (colon && value->is_empty());
        auto expanded_word = [&](String& result) { return m_shell.expand_single(word, result); };
        String result;
        switch (op) {
        case '-':
            if (unset_or_empty) {
                if (!expanded_word(result)) {
                    m_failed = true;
                    return;
                }
                expanded(result.view(), quoted);
            } else {
                expanded(value.value(), quoted);
            }
            return;
        case '=':
            if (unset_or_empty) {
                if (!expanded_word(result)) {
                    m_failed = true;
                    return;
                }
                m_shell.set_variable(name, result.view());
                expanded(result.view(), quoted);
            } else {
                expanded(value.value(), quoted);
            }
            return;
        case '?':
            if (unset_or_empty) {
                if (!expanded_word(result))
                    result = string("parameter null or not set"_sv);
                fail(name, result.is_empty() ? "parameter null or not set"_sv : result.view());
                return;
            }
            expanded(value.value(), quoted);
            return;
        case '+':
            if (!unset_or_empty) {
                if (!expanded_word(result)) {
                    m_failed = true;
                    return;
                }
                expanded(result.view(), quoted);
            }
            return;
        default:
            break;
        }
        if (colon) {
            // ${name:offset:length}
            auto colon_position = operation.find(':');
            StringView offset_text = colon_position.has_value() ? operation.substring_view(0, colon_position.value()) : operation;
            i64 offset = 0, count = -1;
            if (!m_shell.evaluate_arithmetic(offset_text, offset)) {
                m_failed = true;
                return;
            }
            if (colon_position.has_value() && !m_shell.evaluate_arithmetic(operation.substring_view(colon_position.value() + 1), count)) {
                m_failed = true;
                return;
            }
            StringView text = value.has_value() ? value.value() : ""_sv;
            if (offset < 0)
                offset = max<i64>(0, static_cast<i64>(text.length()) + offset);
            size_t start = min(static_cast<size_t>(offset), text.length());
            size_t length = count < 0 ? text.length() - start : min(static_cast<size_t>(count), text.length() - start);
            expanded(text.substring_view(start, length), quoted);
            return;
        }
        StringView text = value.has_value() ? value.value() : ""_sv;
        if (op == '#' || op == '%') {
            bool longest = word.starts_with(op);
            StringView pattern_word = longest ? word.substring_view(1) : word;
            String pattern;
            if (!m_shell.expand_pattern(pattern_word, pattern)) {
                m_failed = true;
                return;
            }
            expanded(trim(text, pattern.view(), op == '%', longest).view(), quoted);
            return;
        }
        if (op == '/') {
            // ${name/pattern/replacement}, ${name//pattern/replacement}
            bool all = word.starts_with('/');
            StringView spec = all ? word.substring_view(1) : word;
            auto slash = spec.find('/');
            String pattern, replacement;
            if (!m_shell.expand_pattern(slash.has_value() ? spec.substring_view(0, slash.value()) : spec, pattern)
                || !m_shell.expand_single(slash.has_value() ? spec.substring_view(slash.value() + 1) : ""_sv, replacement)) {
                m_failed = true;
                return;
            }
            StringBuilder output;
            char pattern_text[1024];
            char candidate[4096];
            if (pattern.length() >= sizeof(pattern_text) || text.length() >= sizeof(candidate)) {
                expanded(text, quoted);
                return;
            }
            memcpy(pattern_text, pattern.characters(), pattern.length() + 1);
            size_t position = 0;
            bool replaced = false;
            while (position < text.length()) {
                size_t match_length = 0;
                if (!replaced || all) {
                    for (size_t length = text.length() - position; length > 0; --length) {
                        memcpy(candidate, text.characters_without_null_termination() + position, length);
                        candidate[length] = '\0';
                        if (fnmatch(pattern_text, candidate, 0) == 0) {
                            match_length = length;
                            break;
                        }
                    }
                }
                if (match_length > 0) {
                    output.append(replacement.view());
                    position += match_length;
                    replaced = true;
                } else {
                    output.append(text[position++]);
                }
            }
            expanded(output.view(), quoted);
            return;
        }
        fail(content, "bad substitution"_sv);
    }

    Shell& m_shell;
    bool m_split;
    bool m_new_field_pending { false };
    bool m_failed { false };
};

// What zsh calls a signal in job messages ("segmentation fault").
char const* signal_text(int signal)
{
    switch (signal) {
    case SIGHUP:
        return "hangup";
    case SIGINT:
        return "interrupt";
    case SIGQUIT:
        return "quit";
    case SIGILL:
        return "illegal hardware instruction";
    case SIGTRAP:
        return "trace trap";
    case SIGABRT:
        return "abort";
    case SIGBUS:
        return "bus error";
    case SIGFPE:
        return "floating point exception";
    case SIGKILL:
        return "killed";
    case SIGUSR1:
        return "user-defined signal 1";
    case SIGSEGV:
        return "segmentation fault";
    case SIGUSR2:
        return "user-defined signal 2";
    case SIGPIPE:
        return "broken pipe";
    case SIGALRM:
        return "alarm";
    case SIGTERM:
        return "terminated";
    default:
        return "signal";
    }
}

// How a job is named in messages: its command, without a final "&".
String job_text(StringView command)
{
    command = command.trim_whitespace();
    if (command.ends_with('&') && !command.ends_with("&&"_sv))
        command = command.substring_view(0, command.length() - 1).trim_whitespace();
    return MUST(String::from_view(command));
}

// A job's state as zsh words it: "running", "suspended", "done",
// "exit 1", "killed", "suspended (tty input)"...
void state_text(Job const& job, char* text, size_t size)
{
    int status = job.status;
    if (job.state == Job::State::Running) {
        snprintf(text, size, "running");
    } else if (job.state == Job::State::Stopped) {
        int signal = WIFSTOPPED(status) ? WSTOPSIG(status) : SIGTSTP;
        snprintf(text, size, "%s", signal == SIGTTIN ? "suspended (tty input)" : signal == SIGTTOU ? "suspended (tty output)" : signal == SIGSTOP ? "suspended (signal)" : "suspended");
    } else if (WIFSIGNALED(status)) {
        snprintf(text, size, "%s%s", signal_text(WTERMSIG(status)), WCOREDUMP(status) ? " (core dumped)" : "");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        snprintf(text, size, "exit %d", WEXITSTATUS(status));
    } else {
        snprintf(text, size, "done");
    }
}

// How zsh reports a foreground job that died of a signal (nothing for ^C
// or a broken pipe): "vsh: segmentation fault  ./crash".
void print_status_message(Job const& job)
{
    if (!WIFSIGNALED(job.status))
        return;
    int signal = WTERMSIG(job.status);
    if (signal == SIGINT || signal == SIGPIPE)
        return;
    char text[64];
    state_text(job, text, sizeof(text));
    fprintf(stderr, "vsh: %-9s  %s\n", text, job.text.characters());
}

int exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    if (WIFSTOPPED(status))
        return 128 + WSTOPSIG(status);
    return 1;
}

int const s_job_signals[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD };

void reset_job_signals()
{
    for (int signal : s_job_signals)
        ::signal(signal, SIG_DFL);
}

}

Shell::Shell()
{
    for (char** entry = environ; entry && *entry; ++entry) {
        StringView text(*entry);
        auto equals = text.find('=');
        if (!equals.has_value())
            continue;
        m_variables.set(string(text.substring_view(0, equals.value())), Variable { string(text.substring_view(equals.value() + 1)), true });
    }
    if (!variable("PATH"_sv).has_value()) {
        set_variable("PATH"_sv, "/usr/bin:/bin:/usr/sbin:/sbin"_sv);
        export_variable("PATH"_sv);
    }
    set_variable("IFS"_sv, " \t\n"_sv);
    set_variable("PS2"_sv, "> "_sv);
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        set_variable("PWD"_sv, StringView(cwd));
        export_variable("PWD"_sv);
    }
    m_pid = getpid();
    StringBuilder parent;
    parent.appendff("{}", getppid());
    set_variable("PPID"_sv, parent.view());
}

void Shell::enable_interactive_mode()
{
    m_interactive = true;
    m_terminal = STDIN_FILENO;
    if (!isatty(m_terminal))
        return;
    // Wait until we are in the foreground, then lead our own group and
    // take the terminal.
    while (tcgetpgrp(m_terminal) != (m_shell_group = getpgrp()))
        kill(-m_shell_group, SIGTTIN);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    m_shell_group = getpid();
    if (getpgrp() != m_shell_group)
        setpgid(0, m_shell_group);
    tcsetpgrp(m_terminal, m_shell_group);
    tcgetattr(m_terminal, &m_terminal_modes);
    m_job_control = true;
}

Optional<StringView> Shell::variable(StringView name) const
{
    if (auto const* found = m_variables.get(name))
        return found->value.view();
    return {};
}

void Shell::set_variable(StringView name, StringView value)
{
    if (auto* found = m_variables.get(name)) {
        found->value = string(value);
        return;
    }
    m_variables.set(string(name), Variable { string(value), false });
}

void Shell::export_variable(StringView name)
{
    if (auto* found = m_variables.get(name)) {
        found->exported = true;
        return;
    }
    m_variables.set(string(name), Variable { String {}, true });
}

void Shell::unset_variable(StringView name)
{
    m_variables.remove(name);
}

Vector<String> Shell::environment() const
{
    Vector<String> result;
    m_variables.for_each([&](String const& name, Variable const& value) {
        if (!value.exported)
            return;
        StringBuilder entry;
        entry.append(name.view());
        entry.append('=');
        entry.append(value.value.view());
        result.append(MUST(entry.to_string()));
    });
    return result;
}

bool Shell::expand_words(Vector<String> const& words, Vector<String>& result)
{
    for (auto const& word : words) {
        Expander expander(*this, true);
        if (!expander.expand(word.view()))
            return false;
        for (auto& field : expander.fields) {
            if (!field.keep)
                continue;
            if (field.has_glob && !noglob) {
                glob_t matches {};
                auto pattern = MUST(field.pattern.to_string());
                if (glob(pattern.characters(), 0, nullptr, &matches) == 0) {
                    for (size_t i = 0; i < matches.gl_pathc; ++i)
                        result.append(string(StringView(matches.gl_pathv[i])));
                    globfree(&matches);
                    continue;
                }
                globfree(&matches);
            }
            result.append(MUST(field.text.to_string()));
        }
    }
    return true;
}

bool Shell::expand_single(StringView raw, String& result)
{
    Expander expander(*this, false);
    if (!expander.expand(raw))
        return false;
    StringBuilder joined;
    for (size_t i = 0; i < expander.fields.size(); ++i) {
        if (i > 0)
            joined.append(' ');
        joined.append(expander.fields[i].text.view());
    }
    result = MUST(joined.to_string());
    return true;
}

bool Shell::expand_pattern(StringView raw, String& result)
{
    Expander expander(*this, false);
    if (!expander.expand(raw))
        return false;
    StringBuilder joined;
    for (auto const& field : expander.fields)
        joined.append(field.pattern.view());
    result = MUST(joined.to_string());
    return true;
}

String Shell::command_substitution(StringView source)
{
    int fds[2];
    if (pipe(fds) < 0)
        return {};
    fflush(stdout);
    pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return {};
    }
    if (child == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        m_in_child = true;
        m_job_control = false;
        m_interactive = false;
        reset_job_signals();
        exit_shell(run_source(source));
    }
    close(fds[1]);
    StringBuilder output;
    char buffer[1024];
    for (;;) {
        ssize_t got = read(fds[0], buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        output.append(StringView(buffer, static_cast<size_t>(got)));
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    m_last_status = exit_code(status);
    StringView text = output.view();
    while (text.ends_with('\n'))
        text = text.substring_view(0, text.length() - 1);
    return string(text);
}

Optional<String> Shell::find_in_path(StringView name) const
{
    if (name.contains('/'))
        return string(name);
    auto path = variable("PATH"_sv);
    StringView search = path.has_value() ? path.value() : "/bin:/usr/bin"_sv;
    Optional<String> found;
    search.for_each_split_view(':', SplitBehavior::KeepEmpty, [&](StringView directory) {
        if (found.has_value())
            return;
        StringBuilder candidate;
        candidate.append(directory.is_empty() ? "."_sv : directory);
        candidate.append('/');
        candidate.append(name);
        auto text = MUST(candidate.to_string());
        struct stat information;
        if (stat(text.characters(), &information) == 0 && S_ISREG(information.st_mode) && access(text.characters(), X_OK) == 0)
            found = move(text);
    });
    return found;
}

void Shell::take_terminal(pid_t group)
{
    if (m_job_control && m_terminal >= 0)
        tcsetpgrp(m_terminal, group);
}

void Shell::forget_job(Job& job)
{
    m_jobs.remove_all_matching([&](OwnPtr<Job> const& candidate) { return candidate.ptr() == &job; });
}

Job* Shell::find_job(StringView specification)
{
    if (m_jobs.is_empty())
        return nullptr;
    if (specification.is_empty() || specification == "%"_sv || specification == "%%"_sv || specification == "%+"_sv)
        return m_jobs.last().ptr();
    if (specification == "%-"_sv)
        return m_jobs.size() > 1 ? m_jobs[m_jobs.size() - 2].ptr() : m_jobs.last().ptr();
    StringView rest = specification.starts_with('%') ? specification.substring_view(1) : specification;
    if (auto number = rest.to_number<int>(); number.has_value()) {
        for (auto& job : m_jobs) {
            if (job->id == number.value())
                return job.ptr();
        }
        return nullptr;
    }
    for (auto& job : m_jobs) {
        if (job->text.view().starts_with(rest))
            return job.ptr();
    }
    return nullptr;
}

int Shell::wait_for_job(Job& job)
{
    job.background = false;
    take_terminal(job.group);
    for (;;) {
        bool all_finished = true;
        for (size_t i = 0; i < job.processes.size(); ++i)
            all_finished = all_finished && job.finished[i];
        if (all_finished) {
            job.state = Job::State::Done;
            break;
        }
        int status = 0;
        pid_t pid;
        if (m_job_control) {
            pid = waitpid(-job.group, &status, WUNTRACED);
        } else {
            pid_t next = 0;
            for (size_t i = 0; i < job.processes.size() && !next; ++i) {
                if (!job.finished[i])
                    next = job.processes[i];
            }
            pid = waitpid(next, &status, 0);
        }
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            // Nothing left to wait for.
            for (size_t i = 0; i < job.processes.size(); ++i)
                job.finished[i] = true;
            continue;
        }
        for (size_t i = 0; i < job.processes.size(); ++i) {
            if (job.processes[i] != pid)
                continue;
            if (WIFSTOPPED(status)) {
                job.state = Job::State::Stopped;
                job.status = status;
            } else {
                job.finished[i] = true;
                if (i == job.processes.size() - 1)
                    job.status = status;
            }
        }
        if (job.state == Job::State::Stopped)
            break;
    }
    if (job.state == Job::State::Stopped && m_job_control && m_terminal >= 0)
        job.has_modes = tcgetattr(m_terminal, &job.modes) == 0;
    take_terminal(m_shell_group);
    if (m_job_control && m_terminal >= 0)
        tcsetattr(m_terminal, TCSADRAIN, &m_terminal_modes);
    if (job.state == Job::State::Stopped) {
        // zsh: "vsh: suspended  sleep 30", under the "^Z" the terminal echoed.
        char text[64];
        state_text(job, text, sizeof(text));
        fprintf(stderr, "\nvsh: %-9s  %s\n", text, job.text.characters());
        return exit_code(job.status);
    }
    int status = job.status;
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT && m_interactive)
        fputc('\n', stderr);
    if (m_interactive)
        print_status_message(job);
    forget_job(job);
    return exit_code(status);
}

void Shell::reap_background_jobs()
{
    Vector<Job*> finished_jobs;
    for (auto& job : m_jobs) {
        for (size_t i = 0; i < job->processes.size(); ++i) {
            if (job->finished[i])
                continue;
            int status = 0;
            pid_t pid = waitpid(job->processes[i], &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (pid <= 0)
                continue;
            if (WIFSTOPPED(status)) {
                job->state = Job::State::Stopped;
            } else if (WIFCONTINUED(status)) {
                job->state = Job::State::Running;
            } else {
                job->finished[i] = true;
                if (i == job->processes.size() - 1)
                    job->status = status;
            }
        }
        bool done = true;
        for (size_t i = 0; i < job->processes.size(); ++i)
            done = done && job->finished[i];
        if (done)
            finished_jobs.append(job.ptr());
    }
    for (auto* job : finished_jobs) {
        job->state = Job::State::Done;
        if (m_interactive)
            print_job(*job, false);
        forget_job(*job);
    }
}

// "[1]  + suspended  sleep 30", as zsh lists a job: "+" the current job
// (the newest), "-" the one before; with `with_pid`, the process too.
void Shell::print_job(Job const& job, bool with_pid, FILE* stream)
{
    char marker = ' ';
    for (size_t i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].ptr() == &job)
            marker = i + 1 == m_jobs.size() ? '+' : (i + 2 == m_jobs.size() ? '-' : ' ');
    }
    char text[64];
    state_text(job, text, sizeof(text));
    if (with_pid)
        fprintf(stream, "[%d]  %c %d %-9s  %s\n", job.id, marker, job.group, text, job.text.characters());
    else
        fprintf(stream, "[%d]  %c %-9s  %s\n", job.id, marker, text, job.text.characters());
}

void Shell::report_jobs(bool everything)
{
    reap_background_jobs();
    for (auto& job : m_jobs)
        print_job(*job, everything, stdout);
}

int Shell::continue_job(Job& job, bool foreground)
{
    // The terminal (as the job left it) is the job's before it runs again.
    if (foreground) {
        take_terminal(job.group);
        if (job.has_modes && m_job_control && m_terminal >= 0)
            tcsetattr(m_terminal, TCSADRAIN, &job.modes);
    }
    if (m_job_control)
        kill(-job.group, SIGCONT);
    else
        for (pid_t pid : job.processes)
            kill(pid, SIGCONT);
    job.state = Job::State::Running;
    // zsh: "[1]  + continued  sleep 30", for fg and bg alike.
    if (m_interactive) {
        char text[] = "continued";
        char marker = m_jobs.size() > 0 && m_jobs.last().ptr() == &job ? '+' : '-';
        fprintf(stderr, "[%d]  %c %-9s  %s\n", job.id, marker, text, job.text.characters());
    }
    if (foreground)
        return wait_for_job(job);
    job.background = true;
    return 0;
}

bool Shell::apply_redirections(Vector<Redirection> const& redirections, Vector<SavedDescriptor>* saved)
{
    for (auto const& redirection : redirections) {
        int fd = -1;
        bool close_target = false;
        String target;
        if (redirection.kind != Redirection::Kind::HereDocument && !expand_single(redirection.target.view(), target))
            return false;
        switch (redirection.kind) {
        case Redirection::Kind::Input:
            fd = open(target.characters(), O_RDONLY);
            break;
        case Redirection::Kind::Output:
        case Redirection::Kind::OutputAndError:
            fd = open(target.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            break;
        case Redirection::Kind::Append:
            fd = open(target.characters(), O_WRONLY | O_CREAT | O_APPEND, 0666);
            break;
        case Redirection::Kind::ReadWrite:
            fd = open(target.characters(), O_RDWR | O_CREAT, 0666);
            break;
        case Redirection::Kind::DuplicateInput:
        case Redirection::Kind::DuplicateOutput:
            if (target.view() == "-"_sv) {
                close_target = true;
                break;
            }
            if (auto number = target.view().to_number<int>(); number.has_value()) {
                fd = number.value();
            } else if (redirection.kind == Redirection::Kind::DuplicateOutput) {
                // ">&file" is "&>file".
                fd = open(target.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            } else {
                fprintf(stderr, "vsh: %s: ambiguous redirect\n", target.characters());
                return false;
            }
            break;
        case Redirection::Kind::HereDocument:
        case Redirection::Kind::HereString: {
            String body;
            if (redirection.kind == Redirection::Kind::HereDocument) {
                if (redirection.literal_body) {
                    body = redirection.body;
                } else {
                    Expander expander(*this, false);
                    expander.here_document = true;
                    if (!expander.expand(redirection.body.view()))
                        return false;
                    StringBuilder text;
                    for (auto const& field : expander.fields)
                        text.append(field.text.view());
                    body = MUST(text.to_string());
                }
            } else {
                StringBuilder text;
                text.append(target.view());
                text.append('\n');
                body = MUST(text.to_string());
            }
            char path[] = "/tmp/vsh-here.XXXXXX";
            fd = mkstemp(path);
            if (fd >= 0) {
                unlink(path);
                (void)write(fd, body.characters(), body.length());
                lseek(fd, 0, SEEK_SET);
            }
            break;
        }
        }
        if (fd < 0 && !close_target) {
            fprintf(stderr, "vsh: %s: %s\n", target.characters(), strerror(errno));
            return false;
        }
        auto save = [&](int which) {
            if (!saved)
                return;
            int copy = fcntl(which, F_DUPFD, 10);
            if (copy >= 0)
                fcntl(copy, F_SETFD, FD_CLOEXEC);
            saved->append({ which, copy });
        };
        save(redirection.fd);
        if (close_target) {
            close(redirection.fd);
            continue;
        }
        if (fd != redirection.fd) {
            if (dup2(fd, redirection.fd) < 0) {
                // "<&5" with nothing open as 5.
                fprintf(stderr, "vsh: %d: %s\n", fd, strerror(errno));
                return false;
            }
            bool is_duplicate = redirection.kind == Redirection::Kind::DuplicateInput || redirection.kind == Redirection::Kind::DuplicateOutput;
            if (!is_duplicate || target.view().to_number<int>().value_or(-1) != fd)
                close(fd);
        }
        if (redirection.kind == Redirection::Kind::OutputAndError) {
            save(STDERR_FILENO);
            dup2(redirection.fd, STDERR_FILENO);
        }
    }
    return true;
}

void Shell::restore_descriptors(Vector<SavedDescriptor>& saved)
{
    fflush(stdout);
    fflush(stderr);
    for (size_t i = saved.size(); i-- > 0;) {
        if (saved[i].saved >= 0) {
            dup2(saved[i].saved, saved[i].fd);
            close(saved[i].saved);
        } else {
            close(saved[i].fd);
        }
    }
    saved.clear();
}

int Shell::run_source(StringView source)
{
    auto parsed = parse(source);
    if (!parsed.program) {
        fprintf(stderr, "vsh: %s\n", parsed.error.characters());
        m_last_status = 2;
        return 2;
    }
    Node const& program = *parsed.program;
    m_programs.append(move(parsed.program));
    int status = execute(program);
    // A program that defined no functions need not be kept.
    m_last_status = status;
    return status;
}

int Shell::run_file(char const* path, Vector<String> arguments)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "vsh: %s: %s\n", path, strerror(errno));
        return 127;
    }
    StringBuilder source;
    char buffer[4096];
    ssize_t got;
    while ((got = read(fd, buffer, sizeof(buffer))) > 0)
        source.append(StringView(buffer, static_cast<size_t>(got)));
    close(fd);
    auto saved = move(m_positional);
    m_positional = move(arguments);
    StringView text = source.view();
    // A "#!" line is a comment to us.
    int status = run_source(text);
    m_positional = move(saved);
    return status;
}

bool Shell::warn_about_suspended_jobs()
{
    if (!m_interactive || m_warned_on_previous_line)
        return false;
    bool suspended = false;
    for (auto const& job : m_jobs)
        suspended = suspended || job->state == Job::State::Stopped;
    if (!suspended)
        return false;
    fprintf(stderr, "vsh: you have suspended jobs.\n");
    m_warned_about_jobs = true;
    return true;
}

[[noreturn]] void Shell::exit_shell(int status)
{
    if (!m_in_child) {
        auto const& trap = m_traps[0];
        if (!trap.is_empty()) {
            String command = trap;
            m_traps[0] = {};
            run_source(command.view());
        }
        // As zsh: the jobs left behind get a hangup (and the suspended
        // ones a continue, so they see it).
        if (m_interactive) {
            for (auto const& job : m_jobs) {
                kill(-job->group, SIGHUP);
                if (job->state == Job::State::Stopped)
                    kill(-job->group, SIGCONT);
            }
        }
    }
    fflush(stdout);
    fflush(stderr);
    if (m_in_child)
        _exit(status);
    exit(status);
}

void Shell::set_trap(int signal, StringView command)
{
    if (signal < 0 || signal >= 65)
        return;
    m_traps[signal] = string(command);
}

StringView Shell::trap_command(int signal) const
{
    if (signal < 0 || signal >= 65)
        return {};
    return m_traps[signal].view();
}

int Shell::execute(Node const& node)
{
    if (m_flow != Flow::Normal)
        return m_last_status;
    Vector<SavedDescriptor> saved;
    bool redirected = node.kind != Node::Kind::Simple && !node.redirections.is_empty();
    if (redirected && !apply_redirections(node.redirections, &saved)) {
        restore_descriptors(saved);
        return m_last_status = 1;
    }
    int status = 0;
    switch (node.kind) {
    case Node::Kind::Sequence:
        status = execute_list(node);
        break;
    case Node::Kind::Background: {
        auto const& child = *node.children[0];
        if (child.kind == Node::Kind::Pipeline) {
            status = execute_pipeline(child, true);
        } else {
            // a && b &: the whole list in one background process.
            auto job = MUST(try_make<Job>());
            job->id = m_jobs.is_empty() ? 1 : m_jobs.last()->id + 1;
            job->text = job_text(node.text.is_empty() ? child.text.view() : node.text.view());
            job->background = true;
            pid_t pid = fork();
            if (pid == 0) {
                m_in_child = true;
                if (m_job_control)
                    setpgid(0, 0);
                m_job_control = false;
                m_interactive = false;
                reset_job_signals();
                exit_shell(execute(child));
            }
            if (m_job_control)
                setpgid(pid, pid);
            job->group = pid;
            job->processes.append(pid);
            job->finished.append(false);
            m_last_background = pid;
            if (m_interactive)
                fprintf(stderr, "[%d] %d\n", job->id, pid);
            m_jobs.append(move(job));
            StringBuilder last;
            last.appendff("{}", pid);
            set_variable("!"_sv, last.view());
        }
        status = 0;
        break;
    }
    case Node::Kind::Pipeline:
        status = execute_pipeline(node, false);
        break;
    case Node::Kind::Not:
        status = execute(*node.children[0]) == 0 ? 1 : 0;
        break;
    case Node::Kind::And:
        status = execute(*node.children[0]);
        if (status == 0 && m_flow == Flow::Normal)
            status = execute(*node.children[1]);
        break;
    case Node::Kind::Or:
        status = execute(*node.children[0]);
        if (status != 0 && m_flow == Flow::Normal)
            status = execute(*node.children[1]);
        break;
    case Node::Kind::Simple:
        status = execute_simple(node, false);
        break;
    case Node::Kind::If: {
        size_t i = 0;
        bool matched = false;
        for (; i + 1 < node.children.size(); i += 2) {
            if (execute(*node.children[i]) == 0) {
                status = execute(*node.children[i + 1]);
                matched = true;
                break;
            }
            if (m_flow != Flow::Normal)
                break;
        }
        if (!matched && m_flow == Flow::Normal)
            status = i < node.children.size() ? execute(*node.children[i]) : 0;
        break;
    }
    case Node::Kind::While:
    case Node::Kind::Until: {
        ++m_loop_depth;
        for (;;) {
            int condition = execute(*node.children[0]);
            if (m_flow != Flow::Normal && m_flow != Flow::Continue)
                break;
            if ((node.kind == Node::Kind::While) != (condition == 0))
                break;
            status = execute(*node.children[1]);
            if (m_flow == Flow::Break) {
                if (--m_flow_count <= 0)
                    m_flow = Flow::Normal;
                break;
            }
            if (m_flow == Flow::Continue) {
                if (--m_flow_count <= 0)
                    m_flow = Flow::Normal;
                else
                    break;
            }
            if (m_flow != Flow::Normal)
                break;
        }
        --m_loop_depth;
        break;
    }
    case Node::Kind::For: {
        Vector<String> items;
        if (node.has_items) {
            if (!expand_words(node.items, items)) {
                status = 1;
                break;
            }
        } else {
            items = m_positional;
        }
        ++m_loop_depth;
        for (auto const& item : items) {
            set_variable(node.name.view(), item.view());
            status = execute(*node.children[0]);
            if (m_flow == Flow::Break) {
                if (--m_flow_count <= 0)
                    m_flow = Flow::Normal;
                break;
            }
            if (m_flow == Flow::Continue) {
                if (--m_flow_count <= 0)
                    m_flow = Flow::Normal;
                else
                    break;
            }
            if (m_flow != Flow::Normal)
                break;
        }
        --m_loop_depth;
        break;
    }
    case Node::Kind::Case: {
        String subject;
        if (!expand_single(node.name.view(), subject)) {
            status = 1;
            break;
        }
        bool done = false;
        for (size_t i = 0; i < node.children.size() && !done; ++i) {
            for (auto const& raw : node.patterns[i]) {
                String pattern;
                if (!expand_pattern(raw.view(), pattern))
                    continue;
                if (fnmatch(pattern.characters(), subject.characters(), 0) == 0) {
                    status = execute(*node.children[i]);
                    done = true;
                    break;
                }
            }
        }
        break;
    }
    case Node::Kind::Subshell: {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            m_in_child = true;
            m_job_control = false;
            exit_shell(execute(*node.children[0]));
        }
        int child_status = 0;
        while (waitpid(pid, &child_status, 0) < 0 && errno == EINTR) {
        }
        status = exit_code(child_status);
        break;
    }
    case Node::Kind::Group:
        status = execute(*node.children[0]);
        break;
    case Node::Kind::Function:
        m_functions.set(node.name, node.children[0].ptr());
        status = 0;
        break;
    }
    if (redirected)
        restore_descriptors(saved);
    m_last_status = status;
    return status;
}

int Shell::execute_list(Node const& node)
{
    int status = 0;
    for (auto const& item : node.children) {
        if (m_flow != Flow::Normal)
            break;
        status = execute(*item);
        if (errexit && status != 0 && (item->kind == Node::Kind::Pipeline || item->kind == Node::Kind::Simple))
            exit_shell(status);
    }
    return status;
}

int Shell::execute_in_child(Node const& node)
{
    if (node.kind == Node::Kind::Simple)
        exit_shell(execute_simple(node, true));
    exit_shell(execute(node));
}

int Shell::execute_pipeline(Node const& node, bool background)
{
    auto const& commands = node.children;
    if (!background && commands.size() == 1) {
        auto const& command = *commands[0];
        if (command.kind != Node::Kind::Simple)
            return execute(command);
        return execute_simple(command, false);
    }
    fflush(stdout);
    fflush(stderr);
    auto job = MUST(try_make<Job>());
    job->id = m_jobs.is_empty() ? 1 : m_jobs.last()->id + 1;
    job->text = job_text(node.text.view());
    job->background = background;
    int previous_read = -1;
    for (size_t i = 0; i < commands.size(); ++i) {
        int fds[2] = { -1, -1 };
        if (i + 1 < commands.size() && pipe(fds) < 0) {
            perror("vsh: pipe");
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("vsh: fork");
            break;
        }
        if (pid == 0) {
            m_in_child = true;
            if (m_job_control) {
                setpgid(0, job->group);
                if (!background)
                    tcsetpgrp(m_terminal, getpgrp());
            }
            m_job_control = false;
            m_interactive = false;
            reset_job_signals();
            if (previous_read >= 0) {
                dup2(previous_read, STDIN_FILENO);
                close(previous_read);
            }
            if (fds[1] >= 0) {
                dup2(fds[1], STDOUT_FILENO);
                close(fds[1]);
                close(fds[0]);
            }
            execute_in_child(*commands[i]);
        }
        if (m_job_control) {
            if (!job->group)
                job->group = pid;
            setpgid(pid, job->group);
        }
        job->processes.append(pid);
        job->finished.append(false);
        if (previous_read >= 0)
            close(previous_read);
        if (fds[1] >= 0)
            close(fds[1]);
        previous_read = fds[0];
    }
    if (previous_read >= 0)
        close(previous_read);
    if (job->processes.is_empty())
        return 1;
    if (!m_job_control)
        job->group = job->processes[0];
    Job& registered = *job;
    m_jobs.append(move(job));
    if (background) {
        m_last_background = registered.processes.last();
        StringBuilder last;
        last.appendff("{}", m_last_background);
        set_variable("!"_sv, last.view());
        if (m_interactive)
            fprintf(stderr, "[%d] %d\n", registered.id, m_last_background);
        return 0;
    }
    return wait_for_job(registered);
}

int Shell::execute_function(Node const& body, Vector<String> arguments)
{
    auto saved = move(m_positional);
    m_positional = move(arguments);
    ++m_function_depth;
    int status = execute(body);
    --m_function_depth;
    if (m_flow == Flow::Return)
        m_flow = Flow::Normal;
    m_positional = move(saved);
    return m_last_status = status;
}

int Shell::run_external(Vector<String> const& arguments, Vector<String> const& assignments)
{
    // In a child already: becomes the program.
    Vector<char*> argv;
    for (auto const& argument : arguments)
        argv.append(const_cast<char*>(argument.characters()));
    argv.append(nullptr);
    auto variables = environment();
    for (auto const& assignment : assignments)
        variables.append(assignment);
    Vector<char*> envp;
    for (auto const& entry : variables)
        envp.append(const_cast<char*>(entry.characters()));
    envp.append(nullptr);

    auto path = find_in_path(arguments[0].view());
    if (!path.has_value()) {
        fprintf(stderr, "vsh: %s: command not found\n", arguments[0].characters());
        return 127;
    }
    execve(path->characters(), argv.begin(), envp.begin());
    int error = errno;
    if (error == ENOEXEC) {
        // A script without "#!": we run it.
        Vector<String> script_arguments;
        for (size_t i = 1; i < arguments.size(); ++i)
            script_arguments.append(arguments[i]);
        set_script_name(path->view());
        return run_file(path->characters(), move(script_arguments));
    }
    fprintf(stderr, "vsh: %s: %s\n", arguments[0].characters(), strerror(error));
    return error == ENOENT ? 127 : 126;
}

int Shell::execute_simple(Node const& node, bool in_child)
{
    Vector<String> words;
    if (!expand_words(node.words, words))
        return m_last_status = 1;

    Vector<String> assignments;
    for (auto const& raw : node.assignments) {
        auto equals = raw.view().find('=').value();
        String value;
        if (!expand_single(raw.view().substring_view(equals + 1), value))
            return m_last_status = 1;
        StringBuilder entry;
        entry.append(raw.view().substring_view(0, equals));
        entry.append('=');
        entry.append(value.view());
        assignments.append(MUST(entry.to_string()));
    }

    if (words.is_empty()) {
        for (auto const& assignment : assignments) {
            auto equals = assignment.view().find('=').value();
            set_variable(assignment.view().substring_view(0, equals), assignment.view().substring_view(equals + 1));
        }
        if (!node.redirections.is_empty()) {
            Vector<SavedDescriptor> saved;
            bool ok = apply_redirections(node.redirections, in_child ? nullptr : &saved);
            restore_descriptors(saved);
            if (!ok)
                return m_last_status = 1;
        }
        return m_last_status;
    }

    if (xtrace) {
        fputs("+", stderr);
        for (auto const& word : words)
            fprintf(stderr, " %s", word.characters());
        fputc('\n', stderr);
    }

    // Aliases: the first word, as written, replaced by its value.
    if (!node.words.is_empty()) {
        if (auto* alias = m_aliases.get(node.words[0].view()); alias && m_alias_depth < 8) {
            StringBuilder source;
            source.append(alias->view());
            StringView text = node.text.view();
            if (text.starts_with(node.words[0].view()))
                source.append(text.substring_view(node.words[0].length()));
            ++m_alias_depth;
            int status = run_source(source.view());
            --m_alias_depth;
            return status;
        }
    }

    StringView name = words[0].view();
    if (auto* function = m_functions.get(name)) {
        Vector<SavedDescriptor> saved;
        if (!apply_redirections(node.redirections, &saved)) {
            restore_descriptors(saved);
            return m_last_status = 1;
        }
        Vector<String> arguments;
        for (size_t i = 1; i < words.size(); ++i)
            arguments.append(words[i]);
        int status = execute_function(**function, move(arguments));
        restore_descriptors(saved);
        return status;
    }

    if (auto builtin_function = builtin(name); builtin_function.has_value()) {
        bool permanent = name == "exec"_sv && words.size() == 1;
        Vector<SavedDescriptor> saved;
        if (!apply_redirections(node.redirections, permanent || in_child ? nullptr : &saved)) {
            restore_descriptors(saved);
            return m_last_status = 1;
        }
        // Assignments before a builtin last for it only.
        struct Previous {
            String name;
            Optional<String> value;
        };
        Vector<Previous> previous;
        for (auto const& assignment : assignments) {
            auto equals = assignment.view().find('=').value();
            StringView variable_name = assignment.view().substring_view(0, equals);
            auto old = variable(variable_name);
            previous.append({ string(variable_name), old.has_value() ? Optional<String>(string(old.value())) : Optional<String> {} });
            set_variable(variable_name, assignment.view().substring_view(equals + 1));
        }
        int status = run_builtin(*this, builtin_function.value(), words);
        for (auto const& entry : previous) {
            if (entry.value.has_value())
                set_variable(entry.name.view(), entry.value->view());
            else
                unset_variable(entry.name.view());
        }
        restore_descriptors(saved);
        return m_last_status = status;
    }

    if (in_child) {
        if (!apply_redirections(node.redirections, nullptr))
            return 1;
        return run_external(words, assignments);
    }

    // An external command: a job of one process.
    fflush(stdout);
    fflush(stderr);
    auto job = MUST(try_make<Job>());
    job->id = m_jobs.is_empty() ? 1 : m_jobs.last()->id + 1;
    job->text = node.text;
    pid_t pid = fork();
    if (pid < 0) {
        perror("vsh: fork");
        return m_last_status = 1;
    }
    if (pid == 0) {
        m_in_child = true;
        if (m_job_control) {
            setpgid(0, 0);
            tcsetpgrp(m_terminal, getpgrp());
        }
        m_job_control = false;
        m_interactive = false;
        reset_job_signals();
        if (!apply_redirections(node.redirections, nullptr))
            _exit(1);
        _exit(run_external(words, assignments));
    }
    if (m_job_control)
        setpgid(pid, pid);
    job->group = pid;
    job->processes.append(pid);
    job->finished.append(false);
    Job& registered = *job;
    m_jobs.append(move(job));
    return m_last_status = wait_for_job(registered);
}


}
