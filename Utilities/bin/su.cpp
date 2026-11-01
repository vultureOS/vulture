/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <LibCore/Account.h>
#include <LibCore/GetPassword.h>
#include <unistd.h>

// su [-] [-lm] [login [argument...]]: a shell as another user (root if
// none is named), after their password (none needed by root).
//   - or -l  as a login: a fresh environment, in their home
//   -m       the environment unchanged
// The arguments go to the shell: su vulture -c "id".
int main(int argc, char** argv)
{
    bool login = false, keep_environment = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        if (strcmp(argv[first], "-") == 0) {
            login = true;
            continue;
        }
        if (strcmp(argv[first], "--") == 0) {
            ++first;
            break;
        }
        bool known = true;
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'l')
                login = true;
            else if (*option == 'm')
                keep_environment = true;
            else
                known = false;
        }
        if (!known)
            break; // Arguments for the shell, as in "su -c id".
    }
    char const* target = "root";
    if (first < argc && argv[first][0] != '-')
        target = argv[first++];

    auto account = Core::Account::from_name(StringView(target));
    if (account.is_error()) {
        fprintf(stderr, "su: unknown login: %s\n", target);
        return 1;
    }
    if (getuid() != 0) {
        auto password = Core::get_password("Password:"_sv);
        if (password.is_error() || !account.value().authenticate(password.value().view())) {
            fprintf(stderr, "su: Sorry\n");
            return 1;
        }
    }

    StringView shell = account.value().shell().is_empty() ? "/bin/sh"_sv : account.value().shell();
    char shell_path[256];
    snprintf(shell_path, sizeof(shell_path), "%.*s", static_cast<int>(shell.length()), shell.characters_without_null_termination());
    auto set = [](char const* key, StringView value) {
        char text[1100];
        snprintf(text, sizeof(text), "%.*s", static_cast<int>(value.length()), value.characters_without_null_termination());
        setenv(key, text, 1);
    };
    if (login) {
        char term[128] = "vt100";
        if (char const* inherited = getenv("TERM"))
            strlcpy(term, inherited, sizeof(term));
        clearenv();
        setenv("TERM", term, 1);
        setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
    }
    if (login || !keep_environment) {
        set("HOME", account.value().home());
        set("SHELL", shell);
        // As BSD su: USER and LOGNAME change, except to root without -l.
        if (login || account.value().uid() != 0) {
            set("USER", account.value().name());
            set("LOGNAME", account.value().name());
        }
    }
    if (auto became = account.value().become(); became.is_error()) {
        fprintf(stderr, "su: %s\n", strerror(became.error().code()));
        return 1;
    }
    if (login) {
        char home[1024];
        snprintf(home, sizeof(home), "%.*s", static_cast<int>(account.value().home().length()), account.value().home().characters_without_null_termination());
        if (chdir(home) < 0)
            chdir("/");
    }
    char name[256];
    snprintf(name, sizeof(name), "%s%s", login ? "-" : "", last_component(shell_path));
    auto** shell_argv = static_cast<char**>(calloc(static_cast<size_t>(argc - first) + 2, sizeof(char*)));
    shell_argv[0] = name;
    for (int i = first; i < argc; ++i)
        shell_argv[i - first + 1] = argv[i];
    execv(shell_path, shell_argv);
    fprintf(stderr, "su: %s: %s\n", shell_path, strerror(errno));
    return 1;
}
