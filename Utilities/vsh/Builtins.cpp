/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Shell.h"
#include <LibBase/CharacterTypes.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Shell {

namespace {

String string(StringView view)
{
    return MUST(String::from_view(view));
}

// --- $((...)) ---

class Arithmetic {
public:
    Arithmetic(Shell& shell, StringView text)
        : m_shell(shell)
        , m_text(text)
    {
    }

    bool evaluate(i64& result)
    {
        result = assignment();
        skip_spaces();
        if (!m_error && m_position < m_text.length())
            error("syntax error in expression");
        return !m_error;
    }

private:
    void error(char const* message)
    {
        if (!m_error)
            fprintf(stderr, "vsh: %.*s: %s\n", static_cast<int>(m_text.length()), m_text.characters_without_null_termination(), message);
        m_error = true;
    }

    void skip_spaces()
    {
        while (m_position < m_text.length() && Base::is_ascii_space(static_cast<u8>(m_text[m_position])))
            ++m_position;
    }

    bool accept(StringView token)
    {
        skip_spaces();
        if (m_text.substring_view(m_position).starts_with(token)) {
            m_position += token.length();
            return true;
        }
        return false;
    }

    // Accepts `token` unless it starts a longer operator (so "<" is not
    // taken from "<=" or "<<").
    bool accept_alone(StringView token, StringView not_followed_by)
    {
        skip_spaces();
        StringView rest = m_text.substring_view(m_position);
        if (!rest.starts_with(token))
            return false;
        if (rest.length() > token.length() && not_followed_by.contains(rest[token.length()]))
            return false;
        m_position += token.length();
        return true;
    }

    i64 variable_value(StringView name)
    {
        auto value = m_shell.variable(name);
        if (!value.has_value() || value->is_empty())
            return 0;
        Arithmetic nested(m_shell, value.value());
        i64 result = 0;
        if (!nested.evaluate(result))
            m_error = true;
        return result;
    }

    void assign(StringView name, i64 value)
    {
        StringBuilder text;
        text.appendff("{}", value);
        m_shell.set_variable(name, text.view());
    }

    i64 assignment()
    {
        skip_spaces();
        size_t start = m_position;
        if (m_position < m_text.length() && (Base::is_ascii_alpha(static_cast<u8>(m_text[m_position])) || m_text[m_position] == '_')) {
            size_t end = m_position;
            while (end < m_text.length() && (Base::is_ascii_alphanumeric(static_cast<u8>(m_text[end])) || m_text[end] == '_'))
                ++end;
            StringView name = m_text.substring_view(m_position, end - m_position);
            m_position = end;
            skip_spaces();
            static StringView const operators[] = { "+="_sv, "-="_sv, "*="_sv, "/="_sv, "%="_sv, "<<="_sv, ">>="_sv, "&="_sv, "|="_sv, "^="_sv };
            for (auto op : operators) {
                if (m_text.substring_view(m_position).starts_with(op)) {
                    m_position += op.length();
                    i64 right = assignment();
                    i64 left = variable_value(name);
                    i64 value = 0;
                    switch (op[0]) {
                    case '+':
                        value = left + right;
                        break;
                    case '-':
                        value = left - right;
                        break;
                    case '*':
                        value = left * right;
                        break;
                    case '/':
                    case '%':
                        if (right == 0) {
                            error("division by zero");
                            return 0;
                        }
                        value = op[0] == '/' ? left / right : left % right;
                        break;
                    case '<':
                        value = left << right;
                        break;
                    case '>':
                        value = left >> right;
                        break;
                    case '&':
                        value = left & right;
                        break;
                    case '|':
                        value = left | right;
                        break;
                    case '^':
                        value = left ^ right;
                        break;
                    }
                    assign(name, value);
                    return value;
                }
            }
            if (m_position < m_text.length() && m_text[m_position] == '=' && (m_position + 1 >= m_text.length() || m_text[m_position + 1] != '=')) {
                ++m_position;
                i64 value = assignment();
                assign(name, value);
                return value;
            }
            m_position = start;
        }
        return conditional();
    }

    i64 conditional()
    {
        i64 condition = logical_or();
        if (accept("?"_sv)) {
            i64 yes = assignment();
            if (!accept(":"_sv)) {
                error("expected ':'");
                return 0;
            }
            i64 no = assignment();
            return condition ? yes : no;
        }
        return condition;
    }

    i64 logical_or()
    {
        i64 value = logical_and();
        while (accept("||"_sv)) {
            i64 right = logical_and();
            value = value || right;
        }
        return value;
    }

    i64 logical_and()
    {
        i64 value = bit_or();
        while (accept("&&"_sv)) {
            i64 right = bit_or();
            value = value && right;
        }
        return value;
    }

    i64 bit_or()
    {
        i64 value = bit_xor();
        while (accept_alone("|"_sv, "|="_sv))
            value |= bit_xor();
        return value;
    }

    i64 bit_xor()
    {
        i64 value = bit_and();
        while (accept_alone("^"_sv, "="_sv))
            value ^= bit_and();
        return value;
    }

    i64 bit_and()
    {
        i64 value = equality();
        while (accept_alone("&"_sv, "&="_sv))
            value &= equality();
        return value;
    }

    i64 equality()
    {
        i64 value = relational();
        for (;;) {
            if (accept("=="_sv))
                value = value == relational();
            else if (accept("!="_sv))
                value = value != relational();
            else
                return value;
        }
    }

    i64 relational()
    {
        i64 value = shift();
        for (;;) {
            if (accept("<="_sv))
                value = value <= shift();
            else if (accept(">="_sv))
                value = value >= shift();
            else if (accept_alone("<"_sv, "<="_sv))
                value = value < shift();
            else if (accept_alone(">"_sv, ">="_sv))
                value = value > shift();
            else
                return value;
        }
    }

    i64 shift()
    {
        i64 value = additive();
        for (;;) {
            if (accept_alone("<<"_sv, "="_sv))
                value <<= additive();
            else if (accept_alone(">>"_sv, "="_sv))
                value >>= additive();
            else
                return value;
        }
    }

    i64 additive()
    {
        i64 value = multiplicative();
        for (;;) {
            if (accept_alone("+"_sv, "+="_sv))
                value += multiplicative();
            else if (accept_alone("-"_sv, "-="_sv))
                value -= multiplicative();
            else
                return value;
        }
    }

    i64 multiplicative()
    {
        i64 value = power();
        for (;;) {
            char op;
            if (accept_alone("*"_sv, "*="_sv))
                op = '*';
            else if (accept_alone("/"_sv, "="_sv))
                op = '/';
            else if (accept_alone("%"_sv, "="_sv))
                op = '%';
            else
                return value;
            i64 right = power();
            if (op == '*') {
                value *= right;
            } else if (right == 0) {
                error("division by zero");
                return 0;
            } else {
                value = op == '/' ? value / right : value % right;
            }
        }
    }

    i64 power()
    {
        i64 base = unary();
        if (accept("**"_sv)) {
            i64 exponent = power();
            i64 result = 1;
            for (i64 i = 0; i < exponent && i < 64; ++i)
                result *= base;
            return exponent < 0 ? 0 : result;
        }
        return base;
    }

    i64 unary()
    {
        if (accept("++"_sv) || accept("--"_sv)) {
            bool increment = m_text[m_position - 1] == '+';
            skip_spaces();
            size_t end = m_position;
            while (end < m_text.length() && (Base::is_ascii_alphanumeric(static_cast<u8>(m_text[end])) || m_text[end] == '_'))
                ++end;
            StringView name = m_text.substring_view(m_position, end - m_position);
            m_position = end;
            i64 value = variable_value(name) + (increment ? 1 : -1);
            assign(name, value);
            return value;
        }
        if (accept_alone("-"_sv, "="_sv))
            return -unary();
        if (accept_alone("+"_sv, "="_sv))
            return unary();
        if (accept_alone("!"_sv, "="_sv))
            return !unary();
        if (accept("~"_sv))
            return ~unary();
        return primary();
    }

    i64 primary()
    {
        skip_spaces();
        if (accept("("_sv)) {
            i64 value = assignment();
            if (!accept(")"_sv))
                error("missing ')'");
            return value;
        }
        if (m_position >= m_text.length()) {
            error("operand expected");
            return 0;
        }
        char c = m_text[m_position];
        if (Base::is_ascii_digit(static_cast<u8>(c))) {
            int base = 10;
            if (c == '0' && m_position + 1 < m_text.length() && (m_text[m_position + 1] == 'x' || m_text[m_position + 1] == 'X')) {
                base = 16;
                m_position += 2;
            } else if (c == '0') {
                base = 8;
            }
            i64 value = 0;
            while (m_position < m_text.length()) {
                char digit = m_text[m_position];
                int amount;
                if (Base::is_ascii_digit(static_cast<u8>(digit)))
                    amount = digit - '0';
                else if (base == 16 && Base::is_ascii_hex_digit(static_cast<u8>(digit)))
                    amount = static_cast<int>(Base::parse_ascii_hex_digit(static_cast<u8>(digit)));
                else
                    break;
                if (amount >= base) {
                    error("value too great for base");
                    return 0;
                }
                value = value * base + amount;
                ++m_position;
            }
            return value;
        }
        if (Base::is_ascii_alpha(static_cast<u8>(c)) || c == '_' || c == '$') {
            if (c == '$')
                ++m_position;
            size_t end = m_position;
            while (end < m_text.length() && (Base::is_ascii_alphanumeric(static_cast<u8>(m_text[end])) || m_text[end] == '_'))
                ++end;
            StringView name = m_text.substring_view(m_position, end - m_position);
            m_position = end;
            i64 value = variable_value(name);
            if (accept("++"_sv)) {
                assign(name, value + 1);
            } else if (accept("--"_sv)) {
                assign(name, value - 1);
            }
            return value;
        }
        error("syntax error: operand expected");
        return 0;
    }

    Shell& m_shell;
    StringView m_text;
    size_t m_position { 0 };
    bool m_error { false };
};

int to_int(char const* text, bool& ok)
{
    char* end = nullptr;
    long value = strtol(text, &end, 10);
    ok = end && end != text && *end == '\0';
    return static_cast<int>(value);
}

void print_error(char const* builtin, char const* message)
{
    fprintf(stderr, "vsh: %s: %s\n", builtin, message);
}

// --- The builtins ---

int builtin_colon(Shell&, Vector<String>&) { return 0; }
int builtin_true(Shell&, Vector<String>&) { return 0; }
int builtin_false(Shell&, Vector<String>&) { return 1; }

int builtin_cd(Shell& shell, Vector<String>& arguments)
{
    char const* target;
    bool print = false;
    if (arguments.size() < 2) {
        auto home = shell.variable("HOME"_sv);
        if (!home.has_value()) {
            print_error("cd", "HOME not set");
            return 1;
        }
        static String saved;
        saved = string(home.value());
        target = saved.characters();
    } else if (arguments[1].view() == "-"_sv) {
        auto previous = shell.variable("OLDPWD"_sv);
        if (!previous.has_value()) {
            print_error("cd", "OLDPWD not set");
            return 1;
        }
        static String saved;
        saved = string(previous.value());
        target = saved.characters();
        print = true;
    } else {
        target = arguments[1].characters();
    }
    char before[PATH_MAX] = "";
    getcwd(before, sizeof(before));
    if (chdir(target) < 0) {
        fprintf(stderr, "vsh: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    char after[PATH_MAX] = "";
    getcwd(after, sizeof(after));
    shell.set_variable("OLDPWD"_sv, StringView(before));
    shell.set_variable("PWD"_sv, StringView(after));
    shell.export_variable("PWD"_sv);
    shell.export_variable("OLDPWD"_sv);
    if (print)
        printf("%s\n", after);
    return 0;
}

int builtin_pwd(Shell&, Vector<String>&)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("vsh: pwd");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

// echo [-neE] arguments
int builtin_echo(Shell&, Vector<String>& arguments)
{
    bool newline = true, escapes = false;
    size_t i = 1;
    for (; i < arguments.size(); ++i) {
        StringView option = arguments[i].view();
        if (option.length() < 2 || option[0] != '-')
            break;
        bool valid = true;
        for (char c : option.substring_view(1))
            valid = valid && (c == 'n' || c == 'e' || c == 'E');
        if (!valid)
            break;
        for (char c : option.substring_view(1)) {
            if (c == 'n')
                newline = false;
            else if (c == 'e')
                escapes = true;
            else
                escapes = false;
        }
    }
    for (size_t first = i; i < arguments.size(); ++i) {
        if (i > first)
            putchar(' ');
        StringView text = arguments[i].view();
        if (!escapes) {
            fwrite(text.characters_without_null_termination(), 1, text.length(), stdout);
            continue;
        }
        for (size_t j = 0; j < text.length(); ++j) {
            char c = text[j];
            if (c != '\\' || j + 1 >= text.length()) {
                putchar(c);
                continue;
            }
            char next = text[++j];
            switch (next) {
            case 'n':
                putchar('\n');
                break;
            case 't':
                putchar('\t');
                break;
            case 'r':
                putchar('\r');
                break;
            case 'a':
                putchar('\a');
                break;
            case 'b':
                putchar('\b');
                break;
            case 'e':
                putchar('\x1b');
                break;
            case '\\':
                putchar('\\');
                break;
            case 'c':
                fflush(stdout);
                return 0;
            case '0': {
                int value = 0;
                for (int digits = 0; digits < 3 && j + 1 < text.length() && text[j + 1] >= '0' && text[j + 1] <= '7'; ++digits)
                    value = value * 8 + (text[++j] - '0');
                putchar(value);
                break;
            }
            default:
                putchar('\\');
                putchar(next);
            }
        }
    }
    if (newline)
        putchar('\n');
    fflush(stdout);
    return 0;
}

int builtin_exit(Shell& shell, Vector<String>& arguments)
{
    int status = shell.last_status();
    if (arguments.size() > 1) {
        bool ok;
        status = to_int(arguments[1].characters(), ok);
        if (!ok) {
            print_error("exit", "numeric argument required");
            status = 2;
        }
    }
    if (shell.warn_about_suspended_jobs())
        return 1;
    shell.exit_shell(status & 0xff);
}

int builtin_export(Shell& shell, Vector<String>& arguments)
{
    if (arguments.size() < 2 || (arguments.size() == 2 && arguments[1].view() == "-p"_sv)) {
        shell.variables().for_each([](String const& name, Variable const& variable) {
            if (variable.exported)
                printf("export %s=\"%s\"\n", name.characters(), variable.value.characters());
        });
        return 0;
    }
    for (size_t i = 1; i < arguments.size(); ++i) {
        StringView argument = arguments[i].view();
        auto equals = argument.find('=');
        if (equals.has_value()) {
            shell.set_variable(argument.substring_view(0, equals.value()), argument.substring_view(equals.value() + 1));
            shell.export_variable(argument.substring_view(0, equals.value()));
        } else {
            shell.export_variable(argument);
        }
    }
    return 0;
}

int builtin_unset(Shell& shell, Vector<String>& arguments)
{
    for (size_t i = 1; i < arguments.size(); ++i) {
        if (arguments[i].view() == "-v"_sv || arguments[i].view() == "-f"_sv)
            continue;
        shell.unset_variable(arguments[i].view());
    }
    return 0;
}

int builtin_set(Shell& shell, Vector<String>& arguments)
{
    if (arguments.size() == 1) {
        shell.variables().for_each([](String const& name, Variable const& variable) {
            printf("%s='%s'\n", name.characters(), variable.value.characters());
        });
        return 0;
    }
    size_t i = 1;
    for (; i < arguments.size(); ++i) {
        StringView option = arguments[i].view();
        if (option == "--"_sv) {
            ++i;
            break;
        }
        if (option.length() < 2 || (option[0] != '-' && option[0] != '+'))
            break;
        bool enable = option[0] == '-';
        if (option == "-o"_sv || option == "+o"_sv) {
            if (++i >= arguments.size())
                break;
            StringView name = arguments[i].view();
            if (name == "errexit"_sv)
                shell.errexit = enable;
            else if (name == "xtrace"_sv)
                shell.xtrace = enable;
            else if (name == "nounset"_sv)
                shell.nounset = enable;
            else if (name == "noglob"_sv)
                shell.noglob = enable;
            continue;
        }
        for (char c : option.substring_view(1)) {
            if (c == 'e')
                shell.errexit = enable;
            else if (c == 'x')
                shell.xtrace = enable;
            else if (c == 'u')
                shell.nounset = enable;
            else if (c == 'f')
                shell.noglob = enable;
        }
    }
    if (i < arguments.size() || (arguments.size() > 1 && arguments[arguments.size() - 1].view() == "--"_sv)) {
        Vector<String> positional;
        for (; i < arguments.size(); ++i)
            positional.append(arguments[i]);
        shell.set_positional(move(positional));
    }
    return 0;
}

int builtin_shift(Shell& shell, Vector<String>& arguments)
{
    size_t count = 1;
    if (arguments.size() > 1) {
        bool ok;
        int value = to_int(arguments[1].characters(), ok);
        if (!ok || value < 0) {
            print_error("shift", "bad number");
            return 1;
        }
        count = static_cast<size_t>(value);
    }
    auto& positional = shell.positional();
    if (count > positional.size())
        return 1;
    for (size_t i = 0; i < count; ++i)
        positional.remove(0);
    return 0;
}

// read [-r] [-p prompt] name...
int builtin_read(Shell& shell, Vector<String>& arguments)
{
    bool raw = false;
    size_t i = 1;
    for (; i < arguments.size(); ++i) {
        if (arguments[i].view() == "-r"_sv) {
            raw = true;
        } else if (arguments[i].view() == "-p"_sv && i + 1 < arguments.size()) {
            fputs(arguments[++i].characters(), stderr);
        } else {
            break;
        }
    }
    StringBuilder line;
    bool got_any = false;
    for (;;) {
        char c;
        ssize_t got = read(STDIN_FILENO, &c, 1);
        if (got <= 0)
            break;
        got_any = true;
        if (c == '\n')
            break;
        if (c == '\\' && !raw) {
            if (read(STDIN_FILENO, &c, 1) <= 0)
                break;
            if (c == '\n')
                continue;
        }
        line.append(c);
    }
    Vector<String> names;
    for (; i < arguments.size(); ++i)
        names.append(arguments[i]);
    if (names.is_empty())
        names.append(string("REPLY"_sv));
    auto ifs = shell.variable("IFS"_sv);
    StringView separators = ifs.has_value() ? ifs.value() : " \t\n"_sv;
    StringView rest = line.view();
    for (size_t n = 0; n < names.size(); ++n) {
        while (!rest.is_empty() && separators.contains(rest[0]))
            rest = rest.substring_view(1);
        if (n + 1 == names.size()) {
            while (!rest.is_empty() && separators.contains(rest[rest.length() - 1]))
                rest = rest.substring_view(0, rest.length() - 1);
            shell.set_variable(names[n].view(), rest);
            break;
        }
        size_t end = 0;
        while (end < rest.length() && !separators.contains(rest[end]))
            ++end;
        shell.set_variable(names[n].view(), rest.substring_view(0, end));
        rest = rest.substring_view(end);
    }
    return got_any ? 0 : 1;
}

int builtin_test(Shell& shell, Vector<String>& arguments)
{
    size_t end = arguments.size();
    if (arguments[0].view() == "["_sv) {
        if (end < 2 || arguments[end - 1].view() != "]"_sv) {
            print_error("[", "missing ']'");
            return 2;
        }
        --end;
    }
    return shell.evaluate_test(arguments, 1, end);
}

int builtin_alias(Shell& shell, Vector<String>& arguments)
{
    if (arguments.size() == 1) {
        shell.aliases().for_each([](String const& name, String const& value) {
            printf("alias %s='%s'\n", name.characters(), value.characters());
        });
        return 0;
    }
    int status = 0;
    for (size_t i = 1; i < arguments.size(); ++i) {
        StringView argument = arguments[i].view();
        auto equals = argument.find('=');
        if (equals.has_value()) {
            shell.aliases().set(string(argument.substring_view(0, equals.value())), string(argument.substring_view(equals.value() + 1)));
        } else if (auto* value = shell.aliases().get(argument)) {
            printf("alias %s='%s'\n", arguments[i].characters(), value->characters());
        } else {
            fprintf(stderr, "vsh: alias: %s: not found\n", arguments[i].characters());
            status = 1;
        }
    }
    return status;
}

int builtin_unalias(Shell& shell, Vector<String>& arguments)
{
    for (size_t i = 1; i < arguments.size(); ++i) {
        if (arguments[i].view() == "-a"_sv)
            shell.aliases().clear();
        else
            shell.aliases().remove(arguments[i].view());
    }
    return 0;
}

int builtin_jobs(Shell& shell, Vector<String>& arguments)
{
    shell.report_jobs(arguments.size() > 1 && arguments[1].view() == "-l"_sv);
    return 0;
}

int builtin_fg(Shell& shell, Vector<String>& arguments)
{
    auto* job = shell.find_job(arguments.size() > 1 ? arguments[1].view() : StringView {});
    if (!job) {
        print_error(arguments[0].characters(), "no such job");
        return 1;
    }
    return shell.continue_job(*job, arguments[0].view() == "fg"_sv);
}

int builtin_wait(Shell& shell, Vector<String>& arguments)
{
    int status = 0;
    if (arguments.size() == 1) {
        while (!shell.jobs().is_empty()) {
            auto& job = *shell.jobs().first();
            for (size_t i = 0; i < job.processes.size(); ++i) {
                int child_status = 0;
                if (!job.finished[i] && waitpid(job.processes[i], &child_status, 0) == job.processes[i]) {
                    job.finished[i] = true;
                    status = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 128 + WTERMSIG(child_status);
                }
            }
            shell.jobs().remove(0);
        }
        return 0;
    }
    for (size_t i = 1; i < arguments.size(); ++i) {
        pid_t pid;
        if (arguments[i].view().starts_with('%')) {
            auto* job = shell.find_job(arguments[i].view());
            if (!job)
                return 127;
            pid = job->processes.last();
        } else {
            pid = static_cast<pid_t>(atoi(arguments[i].characters()));
        }
        int child_status = 0;
        if (waitpid(pid, &child_status, 0) < 0) {
            status = 127;
            continue;
        }
        status = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 128 + WTERMSIG(child_status);
        for (auto& job : shell.jobs()) {
            for (size_t j = 0; j < job->processes.size(); ++j) {
                if (job->processes[j] == pid)
                    job->finished[j] = true;
            }
        }
    }
    shell.jobs().remove_all_matching([](OwnPtr<Job> const& job) {
        for (bool finished : job->finished) {
            if (!finished)
                return false;
        }
        return true;
    });
    return status;
}

int signal_by_name(char const* name)
{
    if (strncasecmp(name, "SIG", 3) == 0)
        name += 3;
    if (strcasecmp(name, "EXIT") == 0)
        return 0;
    for (int number = 1; number < NSIG; ++number) {
        if (strcasecmp(sys_signame[number], name) == 0)
            return number;
    }
    bool ok;
    int number = to_int(name, ok);
    return ok ? number : -1;
}

// kill [-s signal | -signal] pid|%job...
int builtin_kill(Shell& shell, Vector<String>& arguments)
{
    int signal = SIGTERM;
    size_t i = 1;
    if (i < arguments.size() && arguments[i].view() == "-l"_sv) {
        // As /bin/kill -l: upper-case names, in lines of at most 72 columns,
        // or the name of one signal (or of an exit status 128 + signal).
        if (i + 1 < arguments.size()) {
            bool ok;
            int number = to_int(arguments[i + 1].characters(), ok);
            if (number > 128)
                number -= 128;
            if (!ok || number <= 0 || number >= NSIG) {
                print_error("kill", "unknown signal");
                return 1;
            }
            for (char const* c = sys_signame[number]; *c; ++c)
                putchar(toupper(*c));
            putchar('\n');
            return 0;
        }
        size_t column = 0;
        for (int number = 1; number < NSIG; ++number) {
            size_t length = strlen(sys_signame[number]) + 1;
            if (column > 0 && column + length > 72) {
                putchar('\n');
                column = 0;
            } else if (column > 0) {
                putchar(' ');
            }
            for (char const* c = sys_signame[number]; *c; ++c)
                putchar(toupper(*c));
            column += length;
        }
        putchar('\n');
        return 0;
    }
    if (i < arguments.size() && arguments[i].view() == "-s"_sv && i + 1 < arguments.size()) {
        signal = signal_by_name(arguments[i + 1].characters());
        i += 2;
    } else if (i < arguments.size() && arguments[i].view().starts_with('-') && arguments[i].length() > 1) {
        signal = signal_by_name(arguments[i].characters() + 1);
        ++i;
    }
    if (signal < 0) {
        print_error("kill", "invalid signal");
        return 1;
    }
    int status = 0;
    for (; i < arguments.size(); ++i) {
        if (arguments[i].view().starts_with('%')) {
            auto* job = shell.find_job(arguments[i].view());
            if (!job) {
                fprintf(stderr, "vsh: kill: %s: no such job\n", arguments[i].characters());
                status = 1;
                continue;
            }
            if (kill(-job->group, signal) < 0) {
                for (pid_t pid : job->processes)
                    kill(pid, signal);
            }
            if (signal == SIGCONT)
                job->state = Job::State::Running;
            continue;
        }
        bool ok;
        int pid = to_int(arguments[i].characters(), ok);
        if (!ok || kill(pid, signal) < 0) {
            fprintf(stderr, "vsh: kill: %s: %s\n", arguments[i].characters(), ok ? strerror(errno) : "arguments must be process or job IDs");
            status = 1;
        }
    }
    return status;
}

// type / command -v
int describe_command(Shell& shell, StringView name, bool verbose)
{
    if (shell.aliases().contains(name)) {
        if (verbose)
            printf("%.*s is an alias for %s\n", static_cast<int>(name.length()), name.characters_without_null_termination(), shell.aliases().get(name)->characters());
        else
            printf("alias %.*s\n", static_cast<int>(name.length()), name.characters_without_null_termination());
        return 0;
    }
    if (shell.has_function(name)) {
        printf(verbose ? "%.*s is a shell function\n" : "%.*s\n", static_cast<int>(name.length()), name.characters_without_null_termination());
        return 0;
    }
    if (shell.builtin(name).has_value()) {
        printf(verbose ? "%.*s is a shell builtin\n" : "%.*s\n", static_cast<int>(name.length()), name.characters_without_null_termination());
        return 0;
    }
    auto path = shell.find_in_path(name);
    if (!path.has_value()) {
        if (verbose)
            fprintf(stderr, "vsh: type: %.*s: not found\n", static_cast<int>(name.length()), name.characters_without_null_termination());
        return 1;
    }
    if (verbose)
        printf("%.*s is %s\n", static_cast<int>(name.length()), name.characters_without_null_termination(), path->characters());
    else
        printf("%s\n", path->characters());
    return 0;
}

int builtin_type(Shell& shell, Vector<String>& arguments)
{
    int status = 0;
    for (size_t i = 1; i < arguments.size(); ++i)
        status |= describe_command(shell, arguments[i].view(), true);
    return status;
}

int builtin_command(Shell& shell, Vector<String>& arguments)
{
    if (arguments.size() > 2 && (arguments[1].view() == "-v"_sv || arguments[1].view() == "-V"_sv))
        return describe_command(shell, arguments[2].view(), arguments[1].view() == "-V"_sv);
    if (arguments.size() < 2)
        return 0;
    // Run the command ignoring functions: build a source line.
    StringBuilder source;
    for (size_t i = 1; i < arguments.size(); ++i) {
        source.append(i > 1 ? " '"_sv : "'"_sv);
        for (char c : arguments[i].view()) {
            if (c == '\'')
                source.append("'\\''"_sv);
            else
                source.append(c);
        }
        source.append('\'');
    }
    return shell.run_source(source.view());
}

int builtin_source(Shell& shell, Vector<String>& arguments)
{
    if (arguments.size() < 2) {
        print_error(arguments[0].characters(), "filename argument required");
        return 2;
    }
    char const* path = arguments[1].characters();
    String found;
    if (!strchr(path, '/')) {
        auto in_path = shell.find_in_path(arguments[1].view());
        if (in_path.has_value()) {
            found = in_path.release_value();
            path = found.characters();
        }
    }
    Vector<String> rest;
    for (size_t i = 2; i < arguments.size(); ++i)
        rest.append(arguments[i]);
    if (rest.is_empty())
        rest = shell.positional();
    return shell.run_file(path, move(rest));
}

int builtin_eval(Shell& shell, Vector<String>& arguments)
{
    StringBuilder source;
    for (size_t i = 1; i < arguments.size(); ++i) {
        if (i > 1)
            source.append(' ');
        source.append(arguments[i].view());
    }
    return shell.run_source(source.view());
}

int builtin_exec(Shell& shell, Vector<String>& arguments)
{
    if (arguments.size() < 2)
        return 0;
    auto path = shell.find_in_path(arguments[1].view());
    if (!path.has_value()) {
        fprintf(stderr, "vsh: exec: %s: not found\n", arguments[1].characters());
        return 127;
    }
    Vector<char*> argv;
    for (size_t i = 1; i < arguments.size(); ++i)
        argv.append(const_cast<char*>(arguments[i].characters()));
    argv.append(nullptr);
    auto environment = shell.environment();
    Vector<char*> envp;
    for (auto const& entry : environment)
        envp.append(const_cast<char*>(entry.characters()));
    envp.append(nullptr);
    fflush(stdout);
    execve(path->characters(), argv.begin(), envp.begin());
    fprintf(stderr, "vsh: exec: %s: %s\n", arguments[1].characters(), strerror(errno));
    return 126;
}

int flow_builtin(Shell& shell, Vector<String>& arguments, Shell::Flow flow)
{
    int count = 1;
    if (arguments.size() > 1) {
        bool ok;
        count = to_int(arguments[1].characters(), ok);
        if (!ok || (flow != Shell::Flow::Return && count < 1)) {
            print_error(arguments[0].characters(), "numeric argument required");
            return 1;
        }
    }
    if (flow == Shell::Flow::Return) {
        if (!shell.in_function()) {
            print_error("return", "can only return from a function or sourced script");
            return 1;
        }
        shell.set_flow(flow, 1);
        return arguments.size() > 1 ? count : shell.last_status();
    }
    if (shell.loop_depth() == 0)
        return 0;
    shell.set_flow(flow, min(count, shell.loop_depth()));
    return 0;
}

int builtin_break(Shell& shell, Vector<String>& arguments) { return flow_builtin(shell, arguments, Shell::Flow::Break); }
int builtin_continue(Shell& shell, Vector<String>& arguments) { return flow_builtin(shell, arguments, Shell::Flow::Continue); }
int builtin_return(Shell& shell, Vector<String>& arguments) { return flow_builtin(shell, arguments, Shell::Flow::Return); }

int builtin_history(Shell& shell, Vector<String>&)
{
    auto const& history = shell.history();
    for (size_t i = 0; i < history.size(); ++i)
        printf("%5zu  %s\n", i + 1, history[i].characters());
    return 0;
}

int builtin_umask(Shell&, Vector<String>& arguments)
{
    if (arguments.size() < 2) {
        mode_t mask = umask(0);
        umask(mask);
        printf("%04o\n", mask);
        return 0;
    }
    char* end = nullptr;
    long mask = strtol(arguments[1].characters(), &end, 8);
    if (!end || *end || mask < 0 || mask > 0777) {
        print_error("umask", "invalid mask");
        return 1;
    }
    umask(static_cast<mode_t>(mask));
    return 0;
}

int builtin_trap(Shell& shell, Vector<String>& arguments)
{
    if (arguments.size() == 1) {
        for (int signal = 0; signal < 65; ++signal) {
            auto command = shell.trap_command(signal);
            if (!command.is_empty())
                printf("trap -- '%.*s' %d\n", static_cast<int>(command.length()), command.characters_without_null_termination(), signal);
        }
        return 0;
    }
    StringView command = arguments[1].view();
    bool reset = command == "-"_sv;
    for (size_t i = 2; i < arguments.size(); ++i) {
        int signal = signal_by_name(arguments[i].characters());
        if (signal < 0 || signal >= 65) {
            fprintf(stderr, "vsh: trap: %s: invalid signal\n", arguments[i].characters());
            return 1;
        }
        shell.set_trap(signal, reset ? StringView {} : command);
        if (signal != 0) {
            if (reset)
                ::signal(signal, SIG_DFL);
            else if (command.is_empty())
                ::signal(signal, SIG_IGN);
        }
    }
    return 0;
}

int builtin_local(Shell& shell, Vector<String>& arguments)
{
    // Not scoped yet: a plain assignment.
    for (size_t i = 1; i < arguments.size(); ++i) {
        StringView argument = arguments[i].view();
        auto equals = argument.find('=');
        if (equals.has_value())
            shell.set_variable(argument.substring_view(0, equals.value()), argument.substring_view(equals.value() + 1));
        else if (!shell.variable(argument).has_value())
            shell.set_variable(argument, ""_sv);
    }
    return 0;
}

struct BuiltinEntry {
    StringView name;
    Builtin function;
};

BuiltinEntry const s_builtins[] = {
    { ":"_sv, builtin_colon },
    { "."_sv, builtin_source },
    { "["_sv, builtin_test },
    { "alias"_sv, builtin_alias },
    { "bg"_sv, builtin_fg },
    { "break"_sv, builtin_break },
    { "cd"_sv, builtin_cd },
    { "command"_sv, builtin_command },
    { "continue"_sv, builtin_continue },
    { "echo"_sv, builtin_echo },
    { "eval"_sv, builtin_eval },
    { "exec"_sv, builtin_exec },
    { "exit"_sv, builtin_exit },
    { "export"_sv, builtin_export },
    { "false"_sv, builtin_false },
    { "fg"_sv, builtin_fg },
    { "history"_sv, builtin_history },
    { "jobs"_sv, builtin_jobs },
    { "kill"_sv, builtin_kill },
    { "local"_sv, builtin_local },
    { "pwd"_sv, builtin_pwd },
    { "read"_sv, builtin_read },
    { "return"_sv, builtin_return },
    { "set"_sv, builtin_set },
    { "shift"_sv, builtin_shift },
    { "source"_sv, builtin_source },
    { "test"_sv, builtin_test },
    { "trap"_sv, builtin_trap },
    { "true"_sv, builtin_true },
    { "type"_sv, builtin_type },
    { "umask"_sv, builtin_umask },
    { "unalias"_sv, builtin_unalias },
    { "unset"_sv, builtin_unset },
    { "wait"_sv, builtin_wait },
};

}

Optional<Builtin> Shell::builtin(StringView name) const
{
    for (auto const& entry : s_builtins) {
        if (entry.name == name)
            return entry.function;
    }
    return {};
}

int run_builtin(Shell& shell, Builtin function, Vector<String>& arguments)
{
    int status = function(shell, arguments);
    fflush(stdout);
    fflush(stderr);
    return status;
}

bool Shell::evaluate_arithmetic(StringView expression, i64& result)
{
    if (expression.trim_whitespace().is_empty()) {
        result = 0;
        return true;
    }
    Arithmetic arithmetic(*this, expression);
    return arithmetic.evaluate(result);
}

// test / [: unary file and string tests, binary comparisons, ! ( ) -a -o.
int Shell::evaluate_test(Vector<String> const& arguments, size_t first, size_t end)
{
    struct Evaluator {
        Vector<String> const& arguments;
        size_t position;
        size_t end;
        bool error { false };

        [[nodiscard]] bool at_end() const { return position >= end; }
        StringView peek(size_t ahead = 0) const { return position + ahead < end ? arguments[position + ahead].view() : StringView {}; }

        bool primary()
        {
            if (at_end()) {
                error = true;
                return false;
            }
            if (peek() == "!"_sv) {
                ++position;
                return !primary();
            }
            if (peek() == "("_sv) {
                ++position;
                bool value = disjunction();
                if (peek() != ")"_sv)
                    error = true;
                else
                    ++position;
                return value;
            }
            // Binary operators come second.
            if (end - position >= 3 && is_binary(peek(1)))
                return binary();
            StringView first_word = peek();
            if (first_word.length() == 2 && first_word[0] == '-' && end - position >= 2 && is_unary(first_word))
                return unary();
            ++position;
            return !first_word.is_empty();
        }

        static bool is_binary(StringView op)
        {
            static StringView const operators[] = { "="_sv, "=="_sv, "!="_sv, "<"_sv, ">"_sv, "-eq"_sv, "-ne"_sv, "-lt"_sv, "-le"_sv, "-gt"_sv, "-ge"_sv, "-nt"_sv, "-ot"_sv, "-ef"_sv };
            for (auto candidate : operators) {
                if (op == candidate)
                    return true;
            }
            return false;
        }

        static bool is_unary(StringView op)
        {
            return StringView("bcdefghLprsSuwxznt"_sv).contains(op[1]);
        }

        bool unary()
        {
            char op = peek()[1];
            String operand = arguments[position + 1];
            position += 2;
            if (op == 'z')
                return operand.is_empty();
            if (op == 'n')
                return !operand.is_empty();
            if (op == 't')
                return isatty(atoi(operand.characters()));
            struct stat information;
            int result = op == 'L' || op == 'h' ? lstat(operand.characters(), &information) : stat(operand.characters(), &information);
            if (result < 0)
                return false;
            switch (op) {
            case 'e':
                return true;
            case 'f':
                return S_ISREG(information.st_mode);
            case 'd':
                return S_ISDIR(information.st_mode);
            case 'L':
            case 'h':
                return S_ISLNK(information.st_mode);
            case 'p':
                return S_ISFIFO(information.st_mode);
            case 'c':
                return S_ISCHR(information.st_mode);
            case 'b':
                return S_ISBLK(information.st_mode);
            case 'S':
                return false;
            case 's':
                return information.st_size > 0;
            case 'r':
                return access(operand.characters(), R_OK) == 0;
            case 'w':
                return access(operand.characters(), W_OK) == 0;
            case 'x':
                return access(operand.characters(), X_OK) == 0;
            case 'u':
                return information.st_mode & 04000;
            case 'g':
                return information.st_mode & 02000;
            default:
                return false;
            }
        }

        bool binary()
        {
            String left = arguments[position];
            StringView op = peek(1);
            String right = arguments[position + 2];
            position += 3;
            if (op == "="_sv || op == "=="_sv)
                return left.view() == right.view();
            if (op == "!="_sv)
                return left.view() != right.view();
            if (op == "<"_sv)
                return strcmp(left.characters(), right.characters()) < 0;
            if (op == ">"_sv)
                return strcmp(left.characters(), right.characters()) > 0;
            if (op == "-nt"_sv || op == "-ot"_sv || op == "-ef"_sv) {
                struct stat a, b;
                bool have_a = stat(left.characters(), &a) == 0, have_b = stat(right.characters(), &b) == 0;
                if (op == "-ef"_sv)
                    return have_a && have_b && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
                if (op == "-nt"_sv)
                    return have_a && (!have_b || a.st_mtime > b.st_mtime);
                return have_b && (!have_a || a.st_mtime < b.st_mtime);
            }
            bool ok_left, ok_right;
            long long a = strtoll(left.characters(), nullptr, 10), b = strtoll(right.characters(), nullptr, 10);
            ok_left = !left.is_empty();
            ok_right = !right.is_empty();
            if (!ok_left || !ok_right) {
                fprintf(stderr, "vsh: test: integer expression expected\n");
                error = true;
                return false;
            }
            if (op == "-eq"_sv)
                return a == b;
            if (op == "-ne"_sv)
                return a != b;
            if (op == "-lt"_sv)
                return a < b;
            if (op == "-le"_sv)
                return a <= b;
            if (op == "-gt"_sv)
                return a > b;
            return a >= b;
        }

        bool conjunction()
        {
            bool value = primary();
            while (!at_end() && peek() == "-a"_sv) {
                ++position;
                bool right = primary();
                value = value && right;
            }
            return value;
        }

        bool disjunction()
        {
            bool value = conjunction();
            while (!at_end() && peek() == "-o"_sv) {
                ++position;
                bool right = conjunction();
                value = value || right;
            }
            return value;
        }
    };

    if (first >= end)
        return 1;
    Evaluator evaluator { arguments, first, end };
    bool value = evaluator.disjunction();
    if (evaluator.error || !evaluator.at_end()) {
        if (!evaluator.error)
            fprintf(stderr, "vsh: test: too many arguments\n");
        return 2;
    }
    return value ? 0 : 1;
}

}
