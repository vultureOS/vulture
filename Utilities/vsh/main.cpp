/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "LineEditor.h"
#include "Shell.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// vsh: the vultureOS shell.
//
//   vsh                      interactive on a terminal, else reads stdin
//   vsh -c COMMAND [NAME [ARGUMENT...]]
//   vsh [-l] [-i] [-e] [-x] [SCRIPT [ARGUMENT...]]
//
// A login shell (-l, or argv[0] starting with '-') reads /etc/profile and
// ~/.profile; an interactive one then ~/.vshrc.

namespace Shell {

namespace {

String string(StringView view)
{
    return MUST(String::from_view(view));
}

bool starts_with(StringView text, StringView prefix)
{
    return text.length() >= prefix.length() && text.substring_view(0, prefix.length()) == prefix;
}

Vector<String> complete(void* context, StringView line, size_t word_start, bool command_position)
{
    auto& shell = *static_cast<Shell*>(context);
    StringView word = line.substring_view(word_start);
    Vector<String> candidates;
    auto add = [&](StringView candidate) {
        for (auto const& existing : candidates) {
            if (existing.view() == candidate)
                return;
        }
        candidates.append(string(candidate));
    };
    if (command_position && !word.contains('/')) {
        static StringView const builtins[] = { "alias"_sv, "bg"_sv, "cd"_sv, "echo"_sv, "exit"_sv, "export"_sv, "fg"_sv, "history"_sv, "jobs"_sv,
            "kill"_sv, "pwd"_sv, "read"_sv, "set"_sv, "source"_sv, "test"_sv, "type"_sv, "unset"_sv, "wait"_sv };
        for (auto name : builtins) {
            if (starts_with(name, word))
                add(name);
        }
        shell.aliases().for_each([&](String const& name, String const&) {
            if (starts_with(name.view(), word))
                add(name.view());
        });
        auto path = shell.variable("PATH"_sv);
        if (path.has_value()) {
            path->for_each_split_view(':', SplitBehavior::SkipEmpty, [&](StringView directory_name) {
                auto directory_path = string(directory_name);
                DIR* directory = opendir(directory_path.characters());
                if (!directory)
                    return;
                while (struct dirent* entry = readdir(directory)) {
                    StringView name(entry->d_name);
                    if (name.starts_with('.') || !starts_with(name, word))
                        continue;
                    add(name);
                }
                closedir(directory);
            });
        }
    } else {
        // Files: the directory part as typed, the rest matched.
        auto slash = word.find_last('/');
        StringView directory_part = slash.has_value() ? word.substring_view(0, slash.value() + 1) : StringView {};
        StringView name_part = slash.has_value() ? word.substring_view(slash.value() + 1) : word;
        StringBuilder directory_path;
        if (directory_part.starts_with('~')) {
            auto home = shell.variable("HOME"_sv);
            directory_path.append(home.has_value() ? home.value() : "/"_sv);
            directory_path.append(directory_part.substring_view(1));
        } else {
            directory_path.append(directory_part.is_empty() ? "."_sv : directory_part);
        }
        auto opened = MUST(directory_path.to_string());
        DIR* directory = opendir(opened.characters());
        if (directory) {
            while (struct dirent* entry = readdir(directory)) {
                StringView name(entry->d_name);
                if (name == "."_sv || name == ".."_sv)
                    continue;
                if (name.starts_with('.') && !name_part.starts_with('.'))
                    continue;
                if (!starts_with(name, name_part))
                    continue;
                StringBuilder candidate;
                candidate.append(directory_part);
                candidate.append(name);
                StringBuilder full;
                full.append(opened.view());
                full.append('/');
                full.append(name);
                auto full_path = MUST(full.to_string());
                struct stat information;
                if (stat(full_path.characters(), &information) == 0 && S_ISDIR(information.st_mode))
                    candidate.append('/');
                add(candidate.view());
            }
            closedir(directory);
        }
    }
    // Sorted, for listing.
    for (size_t i = 1; i < candidates.size(); ++i) {
        for (size_t j = i; j > 0 && strcmp(candidates[j].characters(), candidates[j - 1].characters()) < 0; --j) {
            auto swap = move(candidates[j]);
            candidates[j] = move(candidates[j - 1]);
            candidates[j - 1] = move(swap);
        }
    }
    return candidates;
}

// PS1 as bash writes it (\u \h \w \W \$ \n \\), or zsh's default look:
// "user@host directory % ".
String make_prompt(Shell& shell)
{
    char host[64] = "vulture";
    gethostname(host, sizeof(host));
    if (char* dot = strchr(host, '.'))
        *dot = '\0';
    char const* user = "user";
    if (struct passwd* entry = getpwuid(geteuid()))
        user = entry->pw_name;
    char cwd[PATH_MAX] = "?";
    getcwd(cwd, sizeof(cwd));
    auto home = shell.variable("HOME"_sv);
    StringBuilder directory;
    StringView cwd_view(cwd);
    if (home.has_value() && !home->is_empty() && (cwd_view == home.value() || (cwd_view.starts_with(home.value()) && cwd_view[home->length()] == '/'))) {
        directory.append('~');
        directory.append(cwd_view.substring_view(home->length()));
    } else {
        directory.append(cwd_view);
    }
    StringView directory_view = directory.view();
    StringView last_component = directory_view;
    if (directory_view != "/"_sv) {
        if (auto slash = directory_view.find_last('/'); slash.has_value())
            last_component = directory_view.substring_view(slash.value() + 1);
    }
    bool root = geteuid() == 0;

    auto ps1 = shell.variable("PS1"_sv);
    StringBuilder prompt;
    if (!ps1.has_value()) {
        prompt.appendff("{}@{} {} {} ", StringView(user), StringView(host), last_component, root ? "#"_sv : "%"_sv);
        return MUST(prompt.to_string());
    }
    StringView format = ps1.value();
    for (size_t i = 0; i < format.length(); ++i) {
        if (format[i] != '\\' || i + 1 >= format.length()) {
            prompt.append(format[i]);
            continue;
        }
        switch (format[++i]) {
        case 'u':
            prompt.append(StringView(user));
            break;
        case 'h':
            prompt.append(StringView(host));
            break;
        case 'w':
            prompt.append(directory_view);
            break;
        case 'W':
            prompt.append(last_component);
            break;
        case '$':
            prompt.append(root ? '#' : '$');
            break;
        case 'n':
            prompt.append('\n');
            break;
        case 'e':
            prompt.append('\x1b');
            break;
        case '\\':
            prompt.append('\\');
            break;
        default:
            prompt.append('\\');
            prompt.append(format[i]);
        }
    }
    return MUST(prompt.to_string());
}

String history_path(Shell& shell)
{
    auto home = shell.variable("HOME"_sv);
    StringBuilder path;
    path.append(home.has_value() ? home.value() : "/tmp"_sv);
    path.append("/.vsh_history"_sv);
    return MUST(path.to_string());
}

void load_history(Shell& shell)
{
    auto path = history_path(shell);
    FILE* file = fopen(path.characters(), "r");
    if (!file)
        return;
    char* line = nullptr;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, file)) > 0) {
        if (line[length - 1] == '\n')
            line[--length] = '\0';
        if (length > 0)
            shell.history().append(string(StringView(line, static_cast<size_t>(length))));
    }
    free(line);
    fclose(file);
    while (shell.history().size() > 1000)
        shell.history().remove(0);
}

void append_history(Shell& shell, StringView line)
{
    auto& history = shell.history();
    if (line.trim_whitespace().is_empty() || (!history.is_empty() && history.last().view() == line))
        return;
    history.append(string(line));
    auto path = history_path(shell);
    int fd = open(path.characters(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;
    (void)write(fd, line.characters_without_null_termination(), line.length());
    (void)write(fd, "\n", 1);
    close(fd);
}

}

void source_if_present(Shell& shell, char const* path);
void source_if_present(Shell& shell, char const* path)
{
    if (access(path, R_OK) == 0)
        shell.run_file(path, shell.positional());
}

int Shell::run_interactive()
{
    load_history(*this);
    LineEditor editor(m_history, complete, this);
    bool warned_about_jobs = false;
    for (;;) {
        reap_background_jobs();
        auto prompt = make_prompt(*this);
        auto line = editor.read_line(prompt.view());
        if (!line.has_value()) {
            bool stopped = false;
            for (auto const& job : m_jobs)
                stopped = stopped || job->state == Job::State::Stopped;
            if (stopped && !warned_about_jobs) {
                fprintf(stderr, "vsh: you have stopped jobs.\n");
                warned_about_jobs = true;
                continue;
            }
            exit_shell(m_last_status);
        }
        warned_about_jobs = false;
        StringBuilder source;
        source.append(line->view());
        // Keep reading while the command is unfinished (an open quote,
        // a missing "fi"...).
        for (;;) {
            auto parsed = parse(source.view());
            if (!parsed.incomplete)
                break;
            auto ps2 = variable("PS2"_sv);
            auto more = editor.read_line(ps2.has_value() ? ps2.value() : "> "_sv);
            if (!more.has_value())
                break;
            source.append('\n');
            source.append(more->view());
        }
        append_history(*this, source.view());
        if (source.view().trim_whitespace().is_empty())
            continue;
        run_source(source.view());
        // Fresh line if a command left the cursor mid-line, as zsh does.
        fflush(stdout);
    }
}

}

int main(int argc, char** argv)
{
    Shell::Shell shell;
    bool login = argc > 0 && argv[0][0] == '-';
    bool force_interactive = false;
    char const* command = nullptr;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; ++i) {
        StringView option(argv[i]);
        if (option == "--"_sv) {
            ++i;
            break;
        }
        if (option == "--login"_sv) {
            login = true;
            continue;
        }
        for (char c : option.substring_view(1)) {
            switch (c) {
            case 'c':
                if (i + 1 >= argc) {
                    fprintf(stderr, "vsh: -c: option requires an argument\n");
                    return 2;
                }
                command = argv[++i];
                break;
            case 'l':
                login = true;
                break;
            case 'i':
                force_interactive = true;
                break;
            case 'e':
                shell.errexit = true;
                break;
            case 'x':
                shell.xtrace = true;
                break;
            case 's':
                break;
            default:
                fprintf(stderr, "vsh: -%c: invalid option\n", c);
                return 2;
            }
        }
    }

    if (command) {
        if (i < argc)
            shell.set_variable("0"_sv, StringView(argv[i++]));
        Vector<String> positional;
        for (; i < argc; ++i)
            positional.append(MUST(String::from_view(StringView(argv[i]))));
        shell.set_positional(move(positional));
        shell.exit_shell(shell.run_source(StringView(command)));
    }

    if (i < argc && !force_interactive) {
        shell.set_variable("0"_sv, StringView(argv[i]));
        char const* script = argv[i++];
        Vector<String> positional;
        for (; i < argc; ++i)
            positional.append(MUST(String::from_view(StringView(argv[i]))));
        shell.exit_shell(shell.run_file(script, move(positional)));
    }

    bool interactive = force_interactive || isatty(STDIN_FILENO);
    if (!interactive) {
        // Commands from a pipe or file on standard input.
        StringBuilder source;
        char buffer[4096];
        ssize_t got;
        while ((got = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0)
            source.append(StringView(buffer, static_cast<size_t>(got)));
        shell.exit_shell(shell.run_source(source.view()));
    }

    shell.enable_interactive_mode();
    if (login) {
        Shell::source_if_present(shell, "/etc/profile");
        if (auto home = shell.variable("HOME"_sv); home.has_value()) {
            StringBuilder path;
            path.append(home.value());
            path.append("/.profile"_sv);
            Shell::source_if_present(shell, MUST(path.to_string()).characters());
        }
    }
    if (auto home = shell.variable("HOME"_sv); home.has_value()) {
        StringBuilder path;
        path.append(home.value());
        path.append("/.vshrc"_sv);
        Shell::source_if_present(shell, MUST(path.to_string()).characters());
    }
    return shell.run_interactive();
}
