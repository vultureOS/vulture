/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <LibCore/Account.h>
#include <LibCore/GetPassword.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// sudo [-kKv] [-u user] [-s | -i] [command [argument...]]: runs a command
// as root (or -u user), as macOS sets it up: members of "admin" may, after
// their own password; the password is remembered for five minutes on each
// terminal (-k forgets it, -K for every terminal; -v renews it); -s runs
// the shell, -i a login shell of the user.

namespace {

char const* const s_directory = "/var/db/sudo";
constexpr time_t s_remembered_for = 5 * 60;

// /var/db/sudo/<user>/<terminal>: when the password was last given there.
void timestamp_path(char const* user, char* path, size_t size)
{
    char const* terminal = ttyname(STDIN_FILENO);
    char name[64];
    strlcpy(name, terminal ? terminal + (strncmp(terminal, "/dev/", 5) == 0 ? 5 : 0) : "unknown", sizeof(name));
    for (char* c = name; *c; ++c) {
        if (*c == '/')
            *c = '_';
    }
    snprintf(path, size, "%s/%s/%s", s_directory, user, name);
}

bool recently_authenticated(char const* user)
{
    char path[256];
    timestamp_path(user, path, sizeof(path));
    struct stat information;
    if (stat(path, &information) < 0)
        return false;
    time_t now = time(nullptr);
    return information.st_mtime <= now && now - information.st_mtime < s_remembered_for;
}

void remember(char const* user)
{
    char directory[256];
    snprintf(directory, sizeof(directory), "%s/%s", s_directory, user);
    mkdir(s_directory, 0700);
    mkdir(directory, 0700);
    char path[256];
    timestamp_path(user, path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        futimens(fd, nullptr);
        close(fd);
    }
}

void forget(char const* user, bool every_terminal)
{
    char path[256];
    if (!every_terminal) {
        timestamp_path(user, path, sizeof(path));
        unlink(path);
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", s_directory, user);
    // Every terminal's stamp: the directory's files, then itself.
    if (DIR* directory = opendir(path)) {
        while (struct dirent* entry = readdir(directory)) {
            if (entry->d_name[0] == '.')
                continue;
            char file[512];
            snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
            unlink(file);
        }
        closedir(directory);
    }
    rmdir(path);
}

// The first time, the warning macOS gives.
void lecture_once(char const* user)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/lectured", s_directory);
    mkdir(s_directory, 0700);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/lectured/%s", s_directory, user);
    if (access(path, F_OK) == 0)
        return;
    fprintf(stderr,
        "\n"
        "WARNING: Improper use of the sudo command could lead to data loss\n"
        "or the deletion of important system files. Please double-check your\n"
        "typing when using sudo. Type \"man sudo\" for more information.\n"
        "\n"
        "To proceed, enter your password, or type Ctrl-C to abort.\n"
        "\n");
    int fd = open(path, O_WRONLY | O_CREAT, 0600);
    if (fd >= 0)
        close(fd);
}

void usage()
{
    fprintf(stderr, "usage: sudo -h | -K | -k | -V\n"
                    "usage: sudo -v [-k] [-u user]\n"
                    "usage: sudo [-k] [-u user] [-s | -i] [command [arg ...]]\n");
}

}

int main(int argc, char** argv)
{
    bool forget_here = false, forget_everywhere = false, validate = false, shell = false, login_shell = false;
    char const* target_name = "root";
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "--") == 0) {
            ++first;
            break;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'u') {
                // -u user, or -uuser.
                if (option[1])
                    target_name = option + 1;
                else if (first + 1 < argc)
                    target_name = argv[++first];
                else {
                    usage();
                    return 1;
                }
                break;
            }
            switch (*option) {
            case 'k':
                forget_here = true;
                break;
            case 'K':
                forget_everywhere = true;
                break;
            case 'v':
                validate = true;
                break;
            case 's':
                shell = true;
                break;
            case 'i':
                login_shell = true;
                break;
            default:
                usage();
                return 1;
            }
        }
    }
    if (geteuid() != 0) {
        fprintf(stderr, "sudo: effective uid is not 0, is sudo installed setuid root?\n");
        return 1;
    }
    auto caller = Core::Account::from_uid(getuid());
    if (caller.is_error()) {
        fprintf(stderr, "sudo: you do not exist in the passwd database\n");
        return 1;
    }
    char user[64];
    snprintf(user, sizeof(user), "%.*s", static_cast<int>(caller.value().name().length()), caller.value().name().characters_without_null_termination());
    if (forget_here || forget_everywhere) {
        forget(user, forget_everywhere);
        if (first == argc && !validate && !shell && !login_shell)
            return 0;
    }
    if (first == argc && !validate && !shell && !login_shell) {
        usage();
        return 1;
    }

    // %admin ALL=(ALL) ALL
    if (getuid() != 0) {
        if (!caller.value().is_in_group("admin"_sv)) {
            fprintf(stderr, "%s is not in the sudoers file.  This incident will be reported.\n", user);
            return 1;
        }
        if (!recently_authenticated(user)) {
            lecture_once(user);
            bool authenticated = false;
            for (int attempt = 0; attempt < 3 && !authenticated; ++attempt) {
                auto password = Core::get_password("Password:"_sv);
                if (password.is_error())
                    return 1;
                authenticated = caller.value().authenticate(password.value().view());
                if (!authenticated && attempt < 2)
                    fprintf(stderr, "Sorry, try again.\n");
            }
            if (!authenticated) {
                fprintf(stderr, "sudo: 3 incorrect password attempts\n");
                return 1;
            }
        }
        remember(user);
    }
    if (validate && first == argc)
        return 0;

    auto target = Core::Account::from_name(StringView(target_name));
    if (target.is_error()) {
        fprintf(stderr, "sudo: unknown user %s\n", target_name);
        return 1;
    }
    char number[16];
    setenv("SUDO_USER", user, 1);
    snprintf(number, sizeof(number), "%u", getuid());
    setenv("SUDO_UID", number, 1);
    snprintf(number, sizeof(number), "%u", getgid());
    setenv("SUDO_GID", number, 1);
    char command[1024] = {};
    for (int i = first; i < argc; ++i) {
        if (i > first)
            strlcat(command, " ", sizeof(command));
        strlcat(command, argv[i], sizeof(command));
    }
    setenv("SUDO_COMMAND", command, 1);
    auto set = [](char const* key, StringView value) {
        char text[1100];
        snprintf(text, sizeof(text), "%.*s", static_cast<int>(value.length()), value.characters_without_null_termination());
        setenv(key, text, 1);
    };
    set("USER", target.value().name());
    set("LOGNAME", target.value().name());
    if (auto became = target.value().become(); became.is_error()) {
        fprintf(stderr, "sudo: %s\n", strerror(became.error().code()));
        return 1;
    }

    if (shell || login_shell) {
        char shell_path[256];
        StringView target_shell = target.value().shell().is_empty() ? "/bin/sh"_sv : target.value().shell();
        char const* environment_shell = getenv("SHELL");
        if (shell && environment_shell && *environment_shell)
            strlcpy(shell_path, environment_shell, sizeof(shell_path));
        else
            snprintf(shell_path, sizeof(shell_path), "%.*s", static_cast<int>(target_shell.length()), target_shell.characters_without_null_termination());
        if (login_shell) {
            set("HOME", target.value().home());
            set("SHELL", target_shell);
            char home[1024];
            snprintf(home, sizeof(home), "%.*s", static_cast<int>(target.value().home().length()), target.value().home().characters_without_null_termination());
            if (chdir(home) < 0)
                chdir("/");
        }
        char name[256];
        snprintf(name, sizeof(name), "%s%s", login_shell ? "-" : "", last_component(shell_path));
        char* shell_argv[] = { name, const_cast<char*>(first < argc ? "-c" : nullptr), first < argc ? command : nullptr, nullptr };
        execv(shell_path, shell_argv);
        fprintf(stderr, "sudo: %s: %s\n", shell_path, strerror(errno));
        return 1;
    }
    execvp(argv[first], argv + first);
    fprintf(stderr, "sudo: %s: command not found\n", argv[first]);
    return 1;
}
