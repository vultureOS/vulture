/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <LibCore/Account.h>
#include <LibCore/GetPassword.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// login [-fp] [user]: asks for a name and password, then starts the
// user's shell in their home, as BSD login does: "Last login: ..." and
// /etc/motd first (unless ~/.hushlogin exists), the home made at the first
// login, the terminal given to the user.
//
//   -f  no password (only root may say so: getty, vlaunchd)
//   -p  keep the environment (TERM is kept regardless)

namespace {

constexpr int max_attempts = 10;
char const* const s_lastlog = "/var/log/lastlog";

// The terminal's name as login records it: "console", "ttys000".
void terminal_line(char* line, size_t size)
{
    char const* name = ttyname(STDIN_FILENO);
    if (!name)
        name = "??";
    if (strncmp(name, "/dev/", 5) == 0)
        name += 5;
    strlcpy(line, name, size);
}

// "Last login: Sun Nov  1 12:34:56 on console", then this login recorded
// in its place. /var/log/lastlog has a line per user: "name time line".
void report_and_record_last_login(char const* name, char const* line, bool quiet)
{
    FILE* in = fopen(s_lastlog, "r");
    char entries[64][128];
    size_t count = 0;
    if (in) {
        char text[128];
        while (count < 64 && fgets(text, sizeof(text), in)) {
            char user[64];
            long long when = 0;
            char where[32];
            if (sscanf(text, "%63s %lld %31s", user, &when, where) == 3 && strcmp(user, name) == 0) {
                if (!quiet) {
                    time_t seconds = static_cast<time_t>(when);
                    struct tm broken;
                    localtime_r(&seconds, &broken);
                    char date[64];
                    strftime(date, sizeof(date), "%a %b %e %H:%M:%S", &broken);
                    printf("Last login: %s on %s\n", date, where);
                }
                continue;
            }
            strlcpy(entries[count++], text, sizeof(entries[0]));
        }
        fclose(in);
    }
    int fd = open(s_lastlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    FILE* out = fdopen(fd, "w");
    for (size_t i = 0; i < count; ++i)
        fputs(entries[i], out);
    fprintf(out, "%s %lld %s\n", name, static_cast<long long>(time(nullptr)), line);
    fclose(out);
}

void print_file(char const* path)
{
    FILE* file = fopen(path, "r");
    if (!file)
        return;
    char buffer[512];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
        fwrite(buffer, 1, got, stdout);
    fclose(file);
}

// The name, from the terminal; nothing at end of input.
bool ask_name(char* name, size_t size)
{
    for (;;) {
        printf("login: ");
        fflush(stdout);
        if (!fgets(name, static_cast<int>(size), stdin))
            return false;
        name[strcspn(name, "\r\n")] = '\0';
        if (name[0])
            return true;
    }
}

}

int main(int argc, char** argv)
{
    bool preauthenticated = false, keep_environment = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; ++first) {
        if (strcmp(argv[first], "--") == 0) {
            ++first;
            break;
        }
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'f')
                preauthenticated = true;
            else if (*option == 'p')
                keep_environment = true;
            else if (*option == 'h' && first + 1 < argc)
                ++first;
            else {
                fprintf(stderr, "usage: login [-fp] [-h hostname] [username]\n");
                return 1;
            }
        }
    }
    if (geteuid() != 0) {
        fprintf(stderr, "login: not installed set-user-ID root\n");
        return 1;
    }
    // Only root may skip the password.
    if (preauthenticated && getuid() != 0)
        preauthenticated = false;
    signal(SIGQUIT, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    char name[64] = {};
    if (first < argc)
        strlcpy(name, argv[first], sizeof(name));
    Optional<Core::Account> account;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (!name[0] && !ask_name(name, sizeof(name)))
            return 0;
        auto found = Core::Account::from_name(StringView(name));
        bool ok;
        if (preauthenticated && attempt == 0) {
            ok = !found.is_error();
        } else {
            // A password is asked for even for a name that does not exist,
            // so the question gives nothing away.
            auto password = Core::get_password("Password:"_sv);
            if (password.is_error())
                return 0;
            ok = !found.is_error() && found.value().authenticate(password.value().view());
        }
        if (ok) {
            account = found.release_value();
            break;
        }
        sleep(1);
        printf("Login incorrect\n");
        fflush(stdout);
        name[0] = '\0';
    }
    if (!account.has_value())
        return 1;

    char line[32];
    terminal_line(line, sizeof(line));
    if (auto made = account->create_home_directory(); made.is_error())
        fprintf(stderr, "login: cannot make %s: %s\n", account->home().characters_without_null_termination(), strerror(made.error().code()));
    // The terminal is the user's now.
    (void)fchown(STDIN_FILENO, account->uid(), account->gid());
    (void)fchmod(STDIN_FILENO, 0620);

    char hush[1100];
    snprintf(hush, sizeof(hush), "%.*s/.hushlogin", static_cast<int>(account->home().length()), account->home().characters_without_null_termination());
    bool quiet = access(hush, F_OK) == 0;
    report_and_record_last_login(name, line, quiet);
    if (!quiet)
        print_file("/etc/motd");
    fflush(stdout);

    // The environment of a fresh login.
    char term[128] = "vt100";
    if (char const* inherited = getenv("TERM"))
        strlcpy(term, inherited, sizeof(term));
    if (!keep_environment)
        clearenv();
    auto set = [](char const* key, StringView value) {
        char text[1100];
        snprintf(text, sizeof(text), "%.*s", static_cast<int>(value.length()), value.characters_without_null_termination());
        setenv(key, text, 1);
    };
    StringView shell = account->shell().is_empty() ? "/bin/sh"_sv : account->shell();
    set("HOME", account->home());
    set("SHELL", shell);
    set("USER", account->name());
    set("LOGNAME", account->name());
    setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
    setenv("TERM", term, 1);

    if (auto became = account->become(); became.is_error()) {
        fprintf(stderr, "login: %s\n", strerror(became.error().code()));
        return 1;
    }
    char home[1024];
    snprintf(home, sizeof(home), "%.*s", static_cast<int>(account->home().length()), account->home().characters_without_null_termination());
    if (chdir(home) < 0) {
        printf("No home directory %s!\nLogging in with home = \"/\".\n", home);
        chdir("/");
        setenv("HOME", "/", 1);
    }
    signal(SIGQUIT, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);

    // A login shell: "-vsh".
    char path[256];
    snprintf(path, sizeof(path), "%.*s", static_cast<int>(shell.length()), shell.characters_without_null_termination());
    char login_name[256];
    snprintf(login_name, sizeof(login_name), "-%s", last_component(path));
    char* shell_argv[] = { login_name, nullptr };
    execv(path, shell_argv);
    fprintf(stderr, "login: %s: %s\n", path, strerror(errno));
    return 1;
}
