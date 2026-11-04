/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <LibBase/HashMap.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// make [-f makefile] [-C directory] [-j n] [-iknrs] [name=value ...] [target ...]: POSIX make, with GNU's handiest parts:
// := and ?=, include, pattern rules, $(wildcard), $(patsubst), $(shell), and the like. -j is accepted, and builds one at a time.

namespace {

struct Variable {
    String value;
    bool from_command_line { false };
    // := has been worked out already. = is worked out when it's used.
    bool is_simple { false };
};

struct Rule {
    Vector<String> prerequisites;
    Vector<String> commands;
    bool is_double_colon { false };
};

struct PatternRule {
    String target;
    Vector<String> prerequisites;
    Vector<String> commands;
};

struct Options {
    bool ignore_errors { false };
    bool keep_going { false };
    bool dry_run { false };
    bool silent { false };
    bool no_builtin { false };
};

Options g_options;
HashMap<String, Variable> g_variables;
HashMap<String, Rule> g_rules;
Vector<PatternRule> g_pattern_rules;
Vector<String> g_rule_order;
HashMap<String, bool> g_phony;
HashMap<String, int> g_built; // 1: being built. 2: done, this run.
int g_recursion_guard = 0;
String g_makefile_list;

[[noreturn]] void fatal(char const* format, char const* detail = "", char const* detail2 = "")
{
    fprintf(stderr, "make: *** ");
    fprintf(stderr, format, detail, detail2);
    fprintf(stderr, ".  Stop.\n");
    exit(2);
}

StringView trim(StringView text)
{
    return text.trim_whitespace();
}

void set_variable(StringView name, StringView value, bool from_command_line, bool is_simple)
{
    auto key = MUST(String::from_view(name));
    if (auto const* existing = g_variables.get(key); existing && existing->from_command_line && !from_command_line)
        return;
    Variable variable;
    variable.value = MUST(String::from_view(value));
    variable.from_command_line = from_command_line;
    variable.is_simple = is_simple;
    MUST(g_variables.try_set(move(key), move(variable)));
}

String expand(StringView text);

// Automatic variables, for the rule being run.
struct Automatic {
    StringView target;
    StringView stem;
    StringView first_prerequisite;
    String all_prerequisites;
    String newer_prerequisites;
};
Automatic const* g_automatic = nullptr;

Vector<String> split_words(StringView text)
{
    Vector<String> words;
    size_t i = 0;
    while (i < text.length()) {
        while (i < text.length() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n'))
            ++i;
        size_t const start = i;
        while (i < text.length() && text[i] != ' ' && text[i] != '\t' && text[i] != '\n')
            ++i;
        if (i > start)
            words.append(MUST(String::from_view(text.substring_view(start, i - start))));
    }
    return words;
}

// Does this name match a pattern with a '%', and if so, what's the '%'?
bool pattern_match(StringView pattern, StringView name, StringView& stem)
{
    auto const percent = pattern.find('%');
    if (!percent.has_value()) {
        stem = {};
        return pattern == name;
    }
    auto const before = pattern.substring_view(0, percent.value());
    auto const after = pattern.substring_view(percent.value() + 1);
    if (name.length() < before.length() + after.length() || !name.starts_with(before) || !name.ends_with(after))
        return false;
    stem = name.substring_view(before.length(), name.length() - before.length() - after.length());
    return true;
}

String apply_pattern(StringView pattern, StringView stem)
{
    auto const percent = pattern.find('%');
    if (!percent.has_value())
        return MUST(String::from_view(pattern));
    StringBuilder out;
    out.append(pattern.substring_view(0, percent.value()));
    out.append(stem);
    out.append(pattern.substring_view(percent.value() + 1));
    return MUST(out.to_string());
}

// The functions: $(name args).
bool call_function(StringView name, StringView raw_arguments, StringBuilder& out)
{
    auto argument_list = [&](size_t wanted) {
        Vector<String> arguments;
        size_t start = 0;
        int depth = 0;
        for (size_t i = 0; i < raw_arguments.length(); ++i) {
            char const c = raw_arguments[i];
            if (c == '(' || c == '{')
                ++depth;
            else if (c == ')' || c == '}')
                --depth;
            else if (c == ',' && depth == 0 && arguments.size() + 1 < wanted) {
                arguments.append(MUST(String::from_view(raw_arguments.substring_view(start, i - start))));
                start = i + 1;
            }
        }
        arguments.append(MUST(String::from_view(raw_arguments.substring_view(start))));
        return arguments;
    };
    auto joined_words = [&](Vector<String> const& words) {
        for (size_t i = 0; i < words.size(); ++i) {
            if (i)
                out.append(' ');
            out.append(words[i].view());
        }
    };
    if (name == "wildcard"_sv) {
        // Through the shell: it knows how to glob, and leaves what doesn't match as it is, which is taken out.
        auto const expanded = expand(raw_arguments);
        auto const words = split_words(expanded.view());
        bool first = true;
        for (auto const& word : words) {
            char command[1200];
            snprintf(command, sizeof(command), "for f in %s; do [ -e \"$f\" ] && echo \"$f\"; done", word.characters());
            FILE* pipe = popen(command, "r");
            if (!pipe)
                continue;
            char line[1024];
            while (fgets(line, sizeof(line), pipe)) {
                line[strcspn(line, "\n")] = 0;
                if (!first)
                    out.append(' ');
                out.append(StringView(line));
                first = false;
            }
            pclose(pipe);
        }
        return true;
    }
    if (name == "shell"_sv) {
        auto const command = expand(raw_arguments);
        fflush(stdout);
        FILE* pipe = popen(command.characters(), "r");
        if (!pipe)
            return true;
        StringBuilder result;
        int c;
        while ((c = fgetc(pipe)) != EOF)
            result.append(c == '\n' ? ' ' : static_cast<char>(c));
        pclose(pipe);
        out.append(trim(result.view()));
        return true;
    }
    if (name == "patsubst"_sv) {
        auto const arguments = argument_list(3);
        if (arguments.size() != 3)
            fatal("patsubst takes three arguments");
        auto const pattern = expand(arguments[0].view());
        auto const replacement = expand(arguments[1].view());
        auto const words = split_words(expand(arguments[2].view()).view());
        for (size_t i = 0; i < words.size(); ++i) {
            if (i)
                out.append(' ');
            StringView stem;
            if (pattern_match(pattern.view(), words[i].view(), stem))
                out.append(apply_pattern(replacement.view(), stem).view());
            else
                out.append(words[i].view());
        }
        return true;
    }
    if (name == "subst"_sv) {
        auto const arguments = argument_list(3);
        if (arguments.size() != 3)
            fatal("subst takes three arguments");
        auto const from = expand(arguments[0].view());
        auto const to = expand(arguments[1].view());
        auto const text = expand(arguments[2].view());
        size_t at = 0;
        while (at < text.view().length()) {
            auto const found = from.view().is_empty() ? Optional<size_t> {} : text.view().find(from.view(), at);
            if (!found.has_value()) {
                out.append(text.view().substring_view(at));
                break;
            }
            out.append(text.view().substring_view(at, found.value() - at));
            out.append(to.view());
            at = found.value() + from.view().length();
        }
        return true;
    }
    if (name == "strip"_sv) {
        joined_words(split_words(expand(raw_arguments).view()));
        return true;
    }
    if (name == "filter"_sv || name == "filter-out"_sv) {
        auto const arguments = argument_list(2);
        if (arguments.size() != 2)
            fatal("filter takes two arguments");
        auto const patterns = split_words(expand(arguments[0].view()).view());
        auto const words = split_words(expand(arguments[1].view()).view());
        bool first = true;
        for (auto const& word : words) {
            bool matched = false;
            for (auto const& pattern : patterns) {
                StringView stem;
                matched = matched || pattern_match(pattern.view(), word.view(), stem);
            }
            if (matched == (name == "filter"_sv)) {
                if (!first)
                    out.append(' ');
                out.append(word.view());
                first = false;
            }
        }
        return true;
    }
    if (name == "addprefix"_sv || name == "addsuffix"_sv) {
        auto const arguments = argument_list(2);
        if (arguments.size() != 2)
            fatal("%s takes two arguments", name == "addprefix"_sv ? "addprefix" : "addsuffix");
        auto const affix = expand(arguments[0].view());
        auto const words = split_words(expand(arguments[1].view()).view());
        for (size_t i = 0; i < words.size(); ++i) {
            if (i)
                out.append(' ');
            if (name == "addprefix"_sv)
                out.append(affix.view());
            out.append(words[i].view());
            if (name == "addsuffix"_sv)
                out.append(affix.view());
        }
        return true;
    }
    if (name == "basename"_sv || name == "suffix"_sv || name == "dir"_sv || name == "notdir"_sv) {
        auto const words = split_words(expand(raw_arguments).view());
        for (size_t i = 0; i < words.size(); ++i) {
            if (i)
                out.append(' ');
            auto const word = words[i].view();
            auto const slash = word.find_last('/');
            if (name == "dir"_sv) {
                if (slash.has_value())
                    out.append(word.substring_view(0, slash.value() + 1));
                else
                    out.append("./"_sv);
                continue;
            }
            if (name == "notdir"_sv) {
                out.append(slash.has_value() ? word.substring_view(slash.value() + 1) : word);
                continue;
            }
            size_t const name_start = slash.has_value() ? slash.value() + 1 : 0;
            auto const dot = word.substring_view(name_start).find_last('.');
            if (name == "basename"_sv)
                out.append(dot.has_value() ? word.substring_view(0, name_start + dot.value()) : word);
            else if (dot.has_value())
                out.append(word.substring_view(name_start + dot.value()));
        }
        return true;
    }
    if (name == "sort"_sv) {
        auto words = split_words(expand(raw_arguments).view());
        for (size_t i = 1; i < words.size(); ++i) {
            for (size_t j = i; j > 0 && words[j].view() < words[j - 1].view(); --j) {
                auto held = move(words[j]);
                words[j] = move(words[j - 1]);
                words[j - 1] = move(held);
            }
        }
        Vector<String> unique;
        for (auto& word : words) {
            if (unique.is_empty() || unique.last().view() != word.view())
                unique.append(move(word));
        }
        joined_words(unique);
        return true;
    }
    if (name == "words"_sv) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%zu", split_words(expand(raw_arguments).view()).size());
        out.append(StringView(buffer));
        return true;
    }
    if (name == "firstword"_sv) {
        auto const words = split_words(expand(raw_arguments).view());
        if (!words.is_empty())
            out.append(words[0].view());
        return true;
    }
    if (name == "if"_sv) {
        auto const arguments = argument_list(3);
        auto const condition = expand(arguments[0].view());
        if (!trim(condition.view()).is_empty()) {
            if (arguments.size() > 1)
                out.append(expand(arguments[1].view()).view());
        } else if (arguments.size() > 2) {
            out.append(expand(arguments[2].view()).view());
        }
        return true;
    }
    if (name == "foreach"_sv) {
        auto const arguments = argument_list(3);
        if (arguments.size() != 3)
            fatal("foreach takes three arguments");
        auto const variable = expand(arguments[0].view());
        auto const words = split_words(expand(arguments[1].view()).view());
        for (size_t i = 0; i < words.size(); ++i) {
            if (i)
                out.append(' ');
            set_variable(variable.view(), words[i].view(), false, true);
            out.append(expand(arguments[2].view()).view());
        }
        return true;
    }
    return false;
}

void expand_reference(StringView reference, StringBuilder& out)
{
    // A function, if it has a space after a name of one.
    auto const space = reference.find(' ');
    if (space.has_value() && call_function(reference.substring_view(0, space.value()), reference.substring_view(space.value() + 1), out))
        return;
    // $(name:from=to)
    auto const colon = reference.find(':');
    auto const equals = reference.find('=');
    if (colon.has_value() && equals.has_value() && equals.value() > colon.value()) {
        auto const base = expand(reference.substring_view(0, colon.value()));
        auto const from = reference.substring_view(colon.value() + 1, equals.value() - colon.value() - 1);
        auto const to = reference.substring_view(equals.value() + 1);
        auto const key = MUST(String::from_view(base.view()));
        auto const* variable = g_variables.get(key);
        if (!variable)
            return;
        auto const value = variable->is_simple ? MUST(String::from_view(variable->value.view())) : expand(variable->value.view());
        auto const words = split_words(value.view());
        for (size_t i = 0; i < words.size(); ++i) {
            if (i)
                out.append(' ');
            StringView stem;
            if (from.contains('%') && pattern_match(from, words[i].view(), stem)) {
                out.append(apply_pattern(to, stem).view());
            } else if (!from.contains('%') && words[i].view().ends_with(from)) {
                out.append(words[i].view().substring_view(0, words[i].view().length() - from.length()));
                out.append(to);
            } else {
                out.append(words[i].view());
            }
        }
        return;
    }
    auto const name = expand(reference);
    auto const* variable = g_variables.get(name);
    if (!variable) {
        // From the environment, as make gets all of them.
        if (char const* from_environment = getenv(name.characters()))
            out.append(StringView(from_environment));
        return;
    }
    if (variable->is_simple) {
        out.append(variable->value.view());
        return;
    }
    if (++g_recursion_guard > 200)
        fatal("Recursive variable '%s' references itself (eventually)", name.characters());
    out.append(expand(variable->value.view()).view());
    --g_recursion_guard;
}

String expand(StringView text)
{
    StringBuilder out;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] != '$' || i + 1 >= text.length()) {
            out.append(text[i]);
            continue;
        }
        char const c = text[++i];
        if (c == '$') {
            out.append('$');
            continue;
        }
        if (c == '(' || c == '{') {
            char const close = c == '(' ? ')' : '}';
            int depth = 1;
            size_t const start = i + 1;
            size_t j = start;
            for (; j < text.length(); ++j) {
                if (text[j] == c)
                    ++depth;
                else if (text[j] == close && --depth == 0)
                    break;
            }
            if (j >= text.length())
                fatal("unterminated variable reference");
            auto const reference = text.substring_view(start, j - start);
            // $(@D) and the like.
            if (reference.length() == 2 && (reference[1] == 'D' || reference[1] == 'F') && g_automatic) {
                StringBuilder one;
                char const name[2] = { reference[0], 0 };
                StringBuilder inner;
                inner.append('$');
                inner.append(name[0]);
                auto const value = expand(inner.view());
                auto const slash = value.view().find_last('/');
                if (reference[1] == 'D')
                    out.append(slash.has_value() ? value.view().substring_view(0, slash.value()) : "."_sv);
                else
                    out.append(slash.has_value() ? value.view().substring_view(slash.value() + 1) : value.view());
            } else {
                expand_reference(reference, out);
            }
            i = j;
            continue;
        }
        // One letter: an automatic variable, usually.
        if (g_automatic) {
            switch (c) {
            case '@':
                out.append(g_automatic->target);
                continue;
            case '<':
                out.append(g_automatic->first_prerequisite);
                continue;
            case '^':
                out.append(g_automatic->all_prerequisites.view());
                continue;
            case '?':
                out.append(g_automatic->newer_prerequisites.view());
                continue;
            case '*':
                out.append(g_automatic->stem);
                continue;
            default:
                break;
            }
        }
        char const reference[2] = { c, 0 };
        expand_reference(StringView(reference), out);
    }
    return MUST(out.to_string());
}

void add_rule(StringView targets_text, StringView prerequisites_text, Vector<String> commands, bool is_double_colon)
{
    auto const targets = split_words(expand(targets_text).view());
    auto const prerequisites = split_words(expand(prerequisites_text).view());
    for (auto const& target : targets) {
        if (target.view() == ".PHONY"_sv) {
            for (auto const& prerequisite : prerequisites)
                MUST(g_phony.try_set(MUST(String::from_view(prerequisite.view())), true));
            continue;
        }
        if (target.view() == ".SILENT"_sv && prerequisites.is_empty()) {
            g_options.silent = true;
            continue;
        }
        if (target.view() == ".IGNORE"_sv && prerequisites.is_empty()) {
            g_options.ignore_errors = true;
            continue;
        }
        if (target.view() == ".SUFFIXES"_sv || target.view() == ".DEFAULT"_sv || target.view() == ".POSIX"_sv)
            continue;
        // ".c.o:": a suffix rule, which is a pattern rule by another name.
        if (target.view().starts_with('.') && prerequisites.is_empty() && !target.view().contains('/')) {
            auto const second_dot = target.view().substring_view(1).find('.');
            if (second_dot.has_value()) {
                auto const from = target.view().substring_view(0, second_dot.value() + 1);
                auto const to = target.view().substring_view(second_dot.value() + 1);
                PatternRule rule;
                StringBuilder target_pattern;
                target_pattern.append('%');
                target_pattern.append(to);
                rule.target = MUST(target_pattern.to_string());
                StringBuilder prerequisite_pattern;
                prerequisite_pattern.append('%');
                prerequisite_pattern.append(from);
                rule.prerequisites.append(MUST(prerequisite_pattern.to_string()));
                for (auto const& command : commands)
                    rule.commands.append(MUST(String::from_view(command.view())));
                g_pattern_rules.append(move(rule));
                continue;
            }
        }
        if (target.view().contains('%')) {
            PatternRule rule;
            rule.target = MUST(String::from_view(target.view()));
            for (auto const& prerequisite : prerequisites)
                rule.prerequisites.append(MUST(String::from_view(prerequisite.view())));
            for (auto const& command : commands)
                rule.commands.append(MUST(String::from_view(command.view())));
            g_pattern_rules.append(move(rule));
            continue;
        }
        if (!g_rules.get(target)) {
            MUST(g_rules.try_set(MUST(String::from_view(target.view())), Rule {}));
            g_rule_order.append(MUST(String::from_view(target.view())));
        }
        auto& rule = *g_rules.get(target);
        for (auto const& prerequisite : prerequisites)
            rule.prerequisites.append(MUST(String::from_view(prerequisite.view())));
        if (!commands.is_empty()) {
            if (!rule.commands.is_empty() && !is_double_colon)
                fprintf(stderr, "make: warning: overriding commands for target `%s'\n", target.characters());
            if (!is_double_colon)
                rule.commands.clear();
            for (auto const& command : commands)
                rule.commands.append(MUST(String::from_view(command.view())));
        }
        rule.is_double_colon = is_double_colon;
    }
}

void read_makefile(char const* path, bool must_exist);

// Conditionals: for each open one, whether its lines count, and whether a branch has been taken.
struct Conditional {
    bool active;
    bool taken;
    bool parent_active;
};

bool evaluate_condition(StringView directive, StringView rest)
{
    if (directive == "ifdef"_sv || directive == "ifndef"_sv) {
        auto const name = expand(trim(rest));
        auto const* variable = g_variables.get(name);
        bool const defined = (variable && !variable->value.view().is_empty()) || getenv(name.characters());
        return directive == "ifdef"_sv ? defined : !defined;
    }
    // ifeq (a,b), or ifeq "a" "b".
    auto const text = trim(rest);
    String left, right;
    if (text.starts_with('(') && text.ends_with(')')) {
        auto const inner = text.substring_view(1, text.length() - 2);
        int depth = 0;
        for (size_t i = 0; i < inner.length(); ++i) {
            if (inner[i] == '(')
                ++depth;
            else if (inner[i] == ')')
                --depth;
            else if (inner[i] == ',' && depth == 0) {
                left = expand(trim(inner.substring_view(0, i)));
                right = expand(trim(inner.substring_view(i + 1)));
                break;
            }
        }
    } else {
        auto const first_end = text.substring_view(1).find(text[0]);
        if (first_end.has_value()) {
            left = expand(text.substring_view(1, first_end.value()));
            auto const remaining = trim(text.substring_view(first_end.value() + 2));
            if (remaining.length() >= 2)
                right = expand(remaining.substring_view(1, remaining.length() - 2));
        }
    }
    bool const equal = left.view() == right.view();
    return directive == "ifeq"_sv ? equal : !equal;
}

void parse_makefile_text(StringView text, char const* name)
{
    Vector<String> lines;
    {
        StringBuilder logical;
        size_t start = 0;
        while (start <= text.length()) {
            auto const newline = start < text.length() ? text.find('\n', start) : Optional<size_t> {};
            size_t const end = newline.has_value() ? newline.value() : text.length();
            auto const physical = text.substring_view(start, end - start);
            if (physical.ends_with('\\')) {
                logical.append(physical.substring_view(0, physical.length() - 1));
                logical.append(' ');
            } else {
                logical.append(physical);
                lines.append(MUST(logical.to_string()));
                logical.clear();
            }
            start = end + 1;
            if (!newline.has_value())
                break;
        }
    }
    Vector<Conditional> conditionals;
    bool in_rule = false;
    String rule_targets;
    String rule_prerequisites;
    Vector<String> rule_commands;
    bool rule_double_colon = false;
    auto finish_rule = [&] {
        if (!in_rule)
            return;
        add_rule(rule_targets.view(), rule_prerequisites.view(), move(rule_commands), rule_double_colon);
        rule_commands = {};
        in_rule = false;
    };
    auto active = [&] { return conditionals.is_empty() || conditionals.last().active; };

    for (size_t number = 0; number < lines.size(); ++number) {
        auto const raw = lines[number].view();
        if (raw.starts_with('\t') && in_rule) {
            if (active())
                rule_commands.append(MUST(String::from_view(raw.substring_view(1))));
            continue;
        }
        // Comments: an unescaped '#'.
        StringBuilder without_comment;
        for (size_t i = 0; i < raw.length(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.length() && raw[i + 1] == '#') {
                without_comment.append('#');
                ++i;
                continue;
            }
            if (raw[i] == '#')
                break;
            without_comment.append(raw[i]);
        }
        auto const owned_line = MUST(without_comment.to_string());
        auto const line = trim(owned_line.view());
        if (line.is_empty())
            continue;

        auto const first_space = line.find(' ');
        auto const first_word = first_space.has_value() ? line.substring_view(0, first_space.value()) : line;
        auto const after_word = first_space.has_value() ? line.substring_view(first_space.value() + 1) : StringView {};
        if (first_word == "ifeq"_sv || first_word == "ifneq"_sv || first_word == "ifdef"_sv || first_word == "ifndef"_sv) {
            finish_rule();
            bool const parent = active();
            bool const condition = parent && evaluate_condition(first_word, after_word);
            conditionals.append({ condition, condition, parent });
            continue;
        }
        if (first_word == "else"_sv) {
            finish_rule();
            if (conditionals.is_empty())
                fatal("%s: extraneous else", name);
            auto& conditional = conditionals.last();
            bool condition = !conditional.taken;
            // else ifeq ...
            auto const nested = trim(after_word);
            if (condition && !nested.is_empty()) {
                auto const nested_space = nested.find(' ');
                auto const nested_word = nested_space.has_value() ? nested.substring_view(0, nested_space.value()) : nested;
                auto const nested_rest = nested_space.has_value() ? nested.substring_view(nested_space.value() + 1) : StringView {};
                condition = evaluate_condition(nested_word, nested_rest);
            }
            conditional.active = conditional.parent_active && condition;
            conditional.taken = conditional.taken || condition;
            continue;
        }
        if (first_word == "endif"_sv) {
            finish_rule();
            if (conditionals.is_empty())
                fatal("%s: extraneous endif", name);
            (void)conditionals.take_last();
            continue;
        }
        if (!active())
            continue;
        if (first_word == "include"_sv || first_word == "-include"_sv || first_word == "sinclude"_sv) {
            finish_rule();
            auto const expanded = expand(after_word);
            for (auto const& file : split_words(expanded.view()))
                read_makefile(file.characters(), first_word == "include"_sv);
            continue;
        }
        if (first_word == "export"_sv || first_word == "unexport"_sv) {
            finish_rule();
            // Everything's exported, when commands run.
            auto const equals = after_word.find('=');
            if (!equals.has_value())
                continue;
        }
        // An assignment?
        size_t equals_at = line.length();
        for (size_t i = 0; i < line.length(); ++i) {
            if (line[i] == '=') {
                equals_at = i;
                break;
            }
            if (line[i] == ':' && !(i + 1 < line.length() && line[i + 1] == '='))
                break;
        }
        if (equals_at < line.length()) {
            finish_rule();
            auto left = line.substring_view(0, equals_at);
            if (left.starts_with("export "_sv))
                left = left.substring_view(7);
            auto const value = trim(line.substring_view(equals_at + 1));
            char kind = '=';
            if (!left.is_empty() && (left[left.length() - 1] == ':' || left[left.length() - 1] == '?' || left[left.length() - 1] == '+')) {
                kind = left[left.length() - 1];
                left = left.substring_view(0, left.length() - 1);
            }
            auto const variable_name = expand(trim(left));
            switch (kind) {
            case ':':
                set_variable(variable_name.view(), expand(value).view(), false, true);
                break;
            case '?':
                if (!g_variables.get(variable_name) && !getenv(variable_name.characters()))
                    set_variable(variable_name.view(), value, false, false);
                break;
            case '+': {
                auto const* existing = g_variables.get(variable_name);
                StringBuilder combined;
                bool const was_simple = existing && existing->is_simple;
                if (existing) {
                    combined.append(existing->value.view());
                    combined.append(' ');
                }
                if (was_simple)
                    combined.append(expand(value).view());
                else
                    combined.append(value);
                set_variable(variable_name.view(), combined.view(), false, was_simple);
                break;
            }
            default:
                set_variable(variable_name.view(), value, false, false);
                break;
            }
            continue;
        }
        // A rule?
        auto const expanded_line = expand(line);
        (void)expanded_line;
        auto const colon = line.find(':');
        if (colon.has_value()) {
            finish_rule();
            in_rule = true;
            rule_targets = MUST(String::from_view(trim(line.substring_view(0, colon.value()))));
            size_t after = colon.value() + 1;
            rule_double_colon = after < line.length() && line[after] == ':';
            if (rule_double_colon)
                ++after;
            auto rest = line.substring_view(after);
            // A ';' starts a command on the same line.
            auto const semicolon = rest.find(';');
            if (semicolon.has_value()) {
                rule_commands.append(MUST(String::from_view(trim(rest.substring_view(semicolon.value() + 1)))));
                rest = rest.substring_view(0, semicolon.value());
            }
            rule_prerequisites = MUST(String::from_view(rest));
            continue;
        }
        char line_number[16];
        snprintf(line_number, sizeof(line_number), "%zu", number + 1);
        fatal("%s:%s: missing separator", name, line_number);
    }
    finish_rule();
    if (!conditionals.is_empty())
        fatal("%s: missing endif", name);
}

void read_makefile(char const* path, bool must_exist)
{
    FILE* file = fopen(path, "r");
    if (!file) {
        if (must_exist)
            fatal("%s: %s", path, strerror(errno));
        return;
    }
    StringBuilder text;
    int c;
    while ((c = fgetc(file)) != EOF)
        text.append(static_cast<char>(c));
    fclose(file);
    auto const whole = MUST(text.to_string());
    parse_makefile_text(whole.view(), path);
}

// ---- Building.

long long modification_time(char const* path)
{
    struct stat information;
    if (stat(path, &information) != 0)
        return -1;
    return static_cast<long long>(information.st_mtime);
}

// Runs one line of a recipe, with @, - and + at the start as they are.
int run_command(StringView command_line)
{
    size_t start = 0;
    bool silent = g_options.silent, ignore = g_options.ignore_errors, always = false;
    while (start < command_line.length()) {
        char const c = command_line[start];
        if (c == '@')
            silent = true;
        else if (c == '-')
            ignore = true;
        else if (c == '+')
            always = true;
        else if (c != ' ' && c != '\t')
            break;
        ++start;
    }
    auto const command = MUST(String::from_view(command_line.substring_view(start)));
    if (command.view().is_empty())
        return 0;
    if (!silent || g_options.dry_run) {
        puts(command.characters());
        fflush(stdout);
    }
    if (g_options.dry_run && !always)
        return 0;
    fflush(stdout);
    pid_t const child = fork();
    if (child == 0) {
        // Make's variables go with it: the ones from the command line, and those exported.
        execl("/bin/sh", "sh", "-c", command.characters(), nullptr);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    int const code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (code != 0) {
        if (ignore) {
            fprintf(stderr, "make: [%s] Error %d (ignored)\n", g_automatic ? MUST(String::from_view(g_automatic->target)).characters() : "", code);
            return 0;
        }
        fprintf(stderr, "make: *** [%s] Error %d\n", g_automatic ? MUST(String::from_view(g_automatic->target)).characters() : "", code);
    }
    return code;
}

int build(String const& target);

// An implicit rule for it, whose prerequisites exist or can be made.
PatternRule const* find_pattern_rule(StringView target, String& stem_out)
{
    for (size_t i = g_pattern_rules.size(); i > 0; --i) {
        auto const& rule = g_pattern_rules[i - 1];
        StringView stem;
        if (!pattern_match(rule.target.view(), target, stem))
            continue;
        bool can_make = true;
        for (auto const& prerequisite : rule.prerequisites) {
            auto const name = apply_pattern(prerequisite.view(), stem);
            if (modification_time(name.characters()) < 0 && !g_rules.get(name)) {
                can_make = false;
                break;
            }
        }
        if (can_make) {
            stem_out = MUST(String::from_view(stem));
            return &rule;
        }
    }
    return nullptr;
}

int build(String const& target)
{
    if (auto const* state = g_built.get(target)) {
        if (*state == 1) {
            fprintf(stderr, "make: Circular %s dependency dropped.\n", target.characters());
            return 0;
        }
        return 0;
    }
    MUST(g_built.try_set(MUST(String::from_view(target.view())), 1));

    auto const* rule = g_rules.get(target);
    String stem;
    PatternRule const* pattern = nullptr;
    bool const has_commands = rule && !rule->commands.is_empty();
    if (!has_commands)
        pattern = find_pattern_rule(target.view(), stem);

    Vector<String> prerequisites;
    if (rule) {
        for (auto const& prerequisite : rule->prerequisites)
            prerequisites.append(MUST(String::from_view(prerequisite.view())));
    }
    if (pattern) {
        Vector<String> from_pattern;
        for (auto const& prerequisite : pattern->prerequisites)
            from_pattern.append(apply_pattern(prerequisite.view(), stem.view()));
        // The pattern's come first: $< is one of them.
        for (auto const& prerequisite : prerequisites)
            from_pattern.append(MUST(String::from_view(prerequisite.view())));
        prerequisites = move(from_pattern);
    }

    long long const target_time = modification_time(target.characters());
    bool const phony = g_phony.get(target) != nullptr;
    if (!rule && !pattern) {
        MUST(g_built.try_set(MUST(String::from_view(target.view())), 2));
        if (target_time >= 0)
            return 0;
        fprintf(stderr, "make: *** No rule to make target `%s'.  Stop.\n", target.characters());
        return 2;
    }

    bool out_of_date = target_time < 0 || phony;
    StringBuilder all;
    StringBuilder newer;
    int failure = 0;
    for (size_t i = 0; i < prerequisites.size(); ++i) {
        int const result = build(prerequisites[i]);
        if (result != 0) {
            failure = result;
            if (!g_options.keep_going)
                return result;
        }
        if (i)
            all.append(' ');
        all.append(prerequisites[i].view());
        long long const prerequisite_time = modification_time(prerequisites[i].characters());
        if (prerequisite_time < 0 || prerequisite_time > target_time) {
            out_of_date = true;
            if (!newer.view().is_empty())
                newer.append(' ');
            newer.append(prerequisites[i].view());
        }
    }
    if (failure)
        return failure;

    MUST(g_built.try_set(MUST(String::from_view(target.view())), 2));
    if (!out_of_date)
        return 0;
    Vector<String> const* commands = has_commands ? &rule->commands : (pattern ? &pattern->commands : nullptr);
    if (!commands || commands->is_empty())
        return 0;

    Automatic automatic;
    automatic.target = target.view();
    automatic.stem = stem.view();
    automatic.first_prerequisite = prerequisites.is_empty() ? StringView {} : prerequisites[0].view();
    automatic.all_prerequisites = MUST(all.to_string());
    automatic.newer_prerequisites = MUST(newer.to_string());
    Automatic const* saved = g_automatic;
    g_automatic = &automatic;
    int result = 0;
    for (auto const& command : *commands) {
        auto const expanded = expand(command.view());
        // A recipe line can have become several.
        size_t start = 0;
        while (start <= expanded.view().length()) {
            auto const newline = start < expanded.view().length() ? expanded.view().find('\n', start) : Optional<size_t> {};
            size_t const end = newline.has_value() ? newline.value() : expanded.view().length();
            result = run_command(expanded.view().substring_view(start, end - start));
            if (result != 0 || !newline.has_value())
                break;
            start = end + 1;
        }
        if (result != 0)
            break;
    }
    g_automatic = saved;
    if (result != 0 && !phony && !g_options.dry_run) {
        // What's half made isn't made.
        fprintf(stderr, "make: *** Deleting file `%s'\n", target.characters());
        (void)unlink(target.characters());
    }
    return result;
}

void define_builtins()
{
    set_variable("CC"_sv, "cc"_sv, false, false);
    set_variable("CXX"_sv, "c++"_sv, false, false);
    set_variable("AR"_sv, "ar"_sv, false, false);
    set_variable("RM"_sv, "rm -f"_sv, false, false);
    set_variable("MAKE"_sv, "make"_sv, false, false);
    set_variable("SHELL"_sv, "/bin/sh"_sv, false, false);
    if (g_options.no_builtin)
        return;
    PatternRule object_from_c;
    object_from_c.target = MUST(String::from_view("%.o"_sv));
    object_from_c.prerequisites.append(MUST(String::from_view("%.c"_sv)));
    object_from_c.commands.append(MUST(String::from_view("$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<"_sv)));
    g_pattern_rules.append(move(object_from_c));
    PatternRule object_from_cpp;
    object_from_cpp.target = MUST(String::from_view("%.o"_sv));
    object_from_cpp.prerequisites.append(MUST(String::from_view("%.cpp"_sv)));
    object_from_cpp.commands.append(MUST(String::from_view("$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<"_sv)));
    g_pattern_rules.append(move(object_from_cpp));
}

int usage()
{
    fprintf(stderr, "usage: make [-iknrs] [-C directory] [-f makefile] [-j jobs] [name=value ...] [target ...]\n");
    return 2;
}

}

int main(int argc, char** argv)
{
    Vector<char const*> makefiles;
    Vector<String> targets;
    Vector<char const*> assignments;
    for (int i = 1; i < argc; ++i) {
        char const* argument = argv[i];
        if (argument[0] == '-' && argument[1]) {
            for (char const* option = argument + 1; *option; ++option) {
                switch (*option) {
                case 'f':
                case 'C':
                case 'j': {
                    char const kind = *option;
                    char const* value = option[1] ? option + 1 : (i + 1 < argc ? argv[++i] : nullptr);
                    if (kind == 'j' && value && !(value[0] >= '0' && value[0] <= '9')) {
                        // "-j" on its own, followed by something else.
                        if (!option[1])
                            --i;
                        value = "1";
                    }
                    if (!value)
                        return usage();
                    if (kind == 'f')
                        makefiles.append(value);
                    else if (kind == 'C' && chdir(value) != 0) {
                        warn("make", value);
                        return 2;
                    }
                    option = "x";
                    break;
                }
                case 'i':
                    g_options.ignore_errors = true;
                    break;
                case 'k':
                    g_options.keep_going = true;
                    break;
                case 'n':
                    g_options.dry_run = true;
                    break;
                case 's':
                    g_options.silent = true;
                    break;
                case 'r':
                    g_options.no_builtin = true;
                    break;
                case 'w':
                    break;
                default:
                    return usage();
                }
            }
        } else if (strchr(argument, '=')) {
            assignments.append(argument);
        } else {
            targets.append(MUST(String::from_view(StringView(argument))));
        }
    }
    define_builtins();
    for (auto const* assignment : assignments) {
        char const* equals = strchr(assignment, '=');
        set_variable(StringView(assignment, static_cast<size_t>(equals - assignment)), StringView(equals + 1), true, true);
        // And for what it runs.
        char name[256];
        snprintf(name, sizeof(name), "%.*s", static_cast<int>(equals - assignment), assignment);
        setenv(name, equals + 1, 1);
    }
    if (makefiles.is_empty()) {
        char const* found = nullptr;
        char const* const candidates[] = { "GNUmakefile", "makefile", "Makefile" };
        for (char const* candidate : candidates) {
            if (access(candidate, F_OK) == 0) {
                found = candidate;
                break;
            }
        }
        if (!found && targets.is_empty()) {
            fprintf(stderr, "make: *** No targets specified and no makefile found.  Stop.\n");
            return 2;
        }
        if (found)
            makefiles.append(found);
    }
    for (auto const* makefile : makefiles)
        read_makefile(makefile, true);
    if (targets.is_empty()) {
        if (g_rule_order.is_empty()) {
            fprintf(stderr, "make: *** No targets.  Stop.\n");
            return 2;
        }
        // The first that isn't special.
        for (auto const& candidate : g_rule_order) {
            if (!candidate.view().starts_with('.')) {
                targets.append(MUST(String::from_view(candidate.view())));
                break;
            }
        }
    }
    int status = 0;
    for (auto const& target : targets) {
        long long const before = modification_time(target.characters());
        bool const had_rule = g_rules.get(target) != nullptr;
        int const result = build(target);
        if (result != 0) {
            status = 2;
            if (!g_options.keep_going)
                break;
            continue;
        }
        // Nothing needed doing.
        if (before >= 0 && modification_time(target.characters()) == before && !g_phony.get(target) && had_rule)
            printf("make: `%s' is up to date.\n", target.characters());
    }
    return status;
}
