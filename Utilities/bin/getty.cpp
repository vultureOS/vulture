/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// getty [type] [terminal]: what vlaunchd keeps running on a terminal. It
// makes the terminal ours, greets with "vultureOS (host) (console)" the
// way macOS's says "Darwin/BSD (host) (console)", asks "login: " and
// hands the name to /usr/bin/login. The type ("std.9600") is accepted for
// launchd plists' sake.
int main(int argc, char** argv)
{
    char const* terminal = argc > 2 ? argv[2] : (argc > 1 && strncmp(argv[1], "std.", 4) != 0 ? argv[1] : "console");
    char path[64];
    snprintf(path, sizeof(path), "%s%s", terminal[0] == '/' ? "" : "/dev/", terminal);

    signal(SIGHUP, SIG_IGN);
    setsid();
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "getty: %s: %s\n", path, strerror(errno));
        sleep(10);
        return 1;
    }
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);
    tcsetpgrp(STDIN_FILENO, getpgrp());
    signal(SIGHUP, SIG_DFL);

    // A sane terminal, whatever the last session left.
    struct termios settings;
    if (tcgetattr(STDIN_FILENO, &settings) == 0) {
        settings.c_iflag = ICRNL | IXON | IMAXBEL;
        settings.c_oflag = OPOST | ONLCR;
        settings.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &settings);
    }

    char host[256] = "localhost";
    gethostname(host, sizeof(host));
    char line[64];
    strlcpy(line, last_component(path), sizeof(line));
    setenv("TERM", "vt100", 1);
    for (;;) {
        printf("\n\nvultureOS (%s) (%s)\n\nlogin: ", host, line);
        fflush(stdout);
        char name[64];
        for (;;) {
            if (!fgets(name, sizeof(name), stdin))
                return 0;
            name[strcspn(name, "\r\n")] = '\0';
            if (name[0])
                break;
            printf("login: ");
            fflush(stdout);
        }
        char* login_argv[] = { const_cast<char*>("login"), const_cast<char*>("-p"), const_cast<char*>("--"), name, nullptr };
        execv("/usr/bin/login", login_argv);
        fprintf(stderr, "getty: /usr/bin/login: %s\n", strerror(errno));
        sleep(5);
    }
}
