/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <libgen.h>
#include <signal.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <termios.h>
#include <unistd.h>

// One program for the small tools, each a name it is installed under:
// true false pwd echo basename dirname readlink realpath mktemp sync clear
// tty nproc logname stty sysctl reboot halt poweroff shutdown.

namespace {

int tool_true(int, char**) { return 0; }
int tool_false(int, char**) { return 1; }

int tool_pwd(int argc, char** argv)
{
    bool logical = !(argc > 1 && strcmp(argv[1], "-P") == 0);
    char const* pwd = getenv("PWD");
    struct stat a, b;
    if (logical && pwd && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 && a.st_ino == b.st_ino && a.st_dev == b.st_dev) {
        printf("%s\n", pwd);
        return 0;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("pwd");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

int tool_echo(int argc, char** argv)
{
    bool newline = true;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = false;
        first = 2;
    }
    for (int i = first; i < argc; ++i)
        printf(i > first ? " %s" : "%s", argv[i]);
    if (newline)
        putchar('\n');
    return 0;
}

int tool_basename(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: basename string [suffix]\n");
        return 1;
    }
    char* name = basename(argv[1]);
    if (argc > 2) {
        size_t length = strlen(name), suffix = strlen(argv[2]);
        if (suffix < length && strcmp(name + length - suffix, argv[2]) == 0)
            name[length - suffix] = '\0';
    }
    printf("%s\n", name);
    return 0;
}

int tool_dirname(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: dirname path\n");
        return 1;
    }
    printf("%s\n", dirname(argv[1]));
    return 0;
}

int tool_readlink(int argc, char** argv)
{
    bool canonical = argc > 2 && strcmp(argv[1], "-f") == 0;
    char const* path = argv[argc - 1];
    if (argc < 2) {
        fprintf(stderr, "usage: readlink [-f] file\n");
        return 1;
    }
    char target[PATH_MAX];
    if (canonical) {
        if (!realpath(path, target))
            return 1;
        printf("%s\n", target);
        return 0;
    }
    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length < 0)
        return 1;
    target[length] = '\0';
    printf("%s\n", target);
    return 0;
}

int tool_realpath(int argc, char** argv)
{
    int status = 0;
    for (int i = 1; i < argc || i == 1; ++i) {
        char const* path = i < argc ? argv[i] : ".";
        char resolved[PATH_MAX];
        if (!realpath(path, resolved)) {
            warn("realpath", path);
            status = 1;
        } else {
            printf("%s\n", resolved);
        }
        if (i >= argc)
            break;
    }
    return status;
}

int tool_mktemp(int argc, char** argv)
{
    bool directory = false, dry_run = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        if (strcmp(argv[first], "-d") == 0)
            directory = true;
        else if (strcmp(argv[first], "-u") == 0)
            dry_run = true;
        else if (strcmp(argv[first], "-t") == 0 && first + 1 < argc)
            ++first;
    }
    char path[PATH_MAX];
    if (first < argc) {
        strlcpy(path, argv[first], sizeof(path));
    } else {
        char const* directory_name = getenv("TMPDIR");
        snprintf(path, sizeof(path), "%s/tmp.XXXXXXXX", directory_name ? directory_name : "/tmp");
    }
    if (directory) {
        if (!mkdtemp(path)) {
            warn("mktemp", path);
            return 1;
        }
    } else {
        int fd = mkstemp(path);
        if (fd < 0) {
            warn("mktemp", path);
            return 1;
        }
        close(fd);
        if (dry_run)
            unlink(path);
    }
    printf("%s\n", path);
    return 0;
}

int tool_sync(int, char**)
{
    sync();
    return 0;
}

int tool_clear(int, char**)
{
    printf("\x1b[H\x1b[2J");
    return 0;
}

int tool_tty(int argc, char** argv)
{
    bool silent = argc > 1 && strcmp(argv[1], "-s") == 0;
    char const* name = ttyname(STDIN_FILENO);
    if (!silent)
        printf("%s\n", name ? name : "not a tty");
    return name ? 0 : 1;
}

int tool_nproc(int, char**)
{
    printf("%ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    return 0;
}

int tool_logname(int, char**)
{
    char const* name = getlogin();
    if (!name) {
        fprintf(stderr, "logname: no login name\n");
        return 1;
    }
    printf("%s\n", name);
    return 0;
}

// stty [-a] | stty [-]echo [-]icanon [-]isig raw sane size rows N cols N
int tool_stty(int argc, char** argv)
{
    struct termios settings;
    if (tcgetattr(STDIN_FILENO, &settings) < 0) {
        perror("stty");
        return 1;
    }
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "-a") == 0)) {
        struct winsize size {};
        ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
        printf("speed 38400 baud; %u rows; %u columns;\n", size.ws_row, size.ws_col);
        printf("lflags: %cicanon %cisig %cecho %cechoe %cechok %ciexten %ctostop\n", settings.c_lflag & ICANON ? ' ' : '-', settings.c_lflag & ISIG ? ' ' : '-',
            settings.c_lflag & ECHO ? ' ' : '-', settings.c_lflag & ECHOE ? ' ' : '-', settings.c_lflag & ECHOK ? ' ' : '-', settings.c_lflag & IEXTEN ? ' ' : '-',
            settings.c_lflag & TOSTOP ? ' ' : '-');
        printf("iflags: %cicrnl %cixon %cistrip\noflags: %copost %connlcr\n", settings.c_iflag & ICRNL ? ' ' : '-', settings.c_iflag & IXON ? ' ' : '-',
            settings.c_iflag & ISTRIP ? ' ' : '-', settings.c_oflag & OPOST ? ' ' : '-', settings.c_oflag & ONLCR ? ' ' : '-');
        printf("cchars: intr = ^%c; quit = ^\\; erase = ^?; kill = ^%c; eof = ^%c; susp = ^%c; werase = ^%c;\n", settings.c_cc[VINTR] + '@',
            settings.c_cc[VKILL] + '@', settings.c_cc[VEOF] + '@', settings.c_cc[VSUSP] + '@', settings.c_cc[VWERASE] + '@');
        return 0;
    }
    struct winsize size {};
    ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
    bool size_changed = false;
    for (int i = 1; i < argc; ++i) {
        char const* word = argv[i];
        bool off = word[0] == '-';
        char const* name = off ? word + 1 : word;
        auto flag = [&](tcflag_t& field, tcflag_t bit) { field = off ? (field & ~bit) : (field | bit); };
        if (strcmp(name, "echo") == 0)
            flag(settings.c_lflag, ECHO);
        else if (strcmp(name, "icanon") == 0)
            flag(settings.c_lflag, ICANON);
        else if (strcmp(name, "isig") == 0)
            flag(settings.c_lflag, ISIG);
        else if (strcmp(name, "tostop") == 0)
            flag(settings.c_lflag, TOSTOP);
        else if (strcmp(name, "icrnl") == 0)
            flag(settings.c_iflag, ICRNL);
        else if (strcmp(name, "opost") == 0)
            flag(settings.c_oflag, OPOST);
        else if (strcmp(name, "raw") == 0)
            cfmakeraw(&settings);
        else if (strcmp(name, "sane") == 0 || strcmp(name, "cooked") == 0) {
            settings.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG | IEXTEN;
            settings.c_iflag |= ICRNL | IXON;
            settings.c_oflag |= OPOST | ONLCR;
        } else if (strcmp(name, "size") == 0) {
            printf("%u %u\n", size.ws_row, size.ws_col);
        } else if (strcmp(name, "rows") == 0 && i + 1 < argc) {
            size.ws_row = static_cast<unsigned short>(atoi(argv[++i]));
            size_changed = true;
        } else if (strcmp(name, "cols") == 0 && i + 1 < argc) {
            size.ws_col = static_cast<unsigned short>(atoi(argv[++i]));
            size_changed = true;
        } else {
            fprintf(stderr, "stty: illegal option -- %s\n", word);
            return 1;
        }
    }
    if (size_changed)
        ioctl(STDIN_FILENO, TIOCSWINSZ, &size);
    return tcsetattr(STDIN_FILENO, TCSADRAIN, &settings) < 0 ? 1 : 0;
}

// sysctl [-a | -n] name...
int tool_sysctl(int argc, char** argv)
{
    static char const* const names[] = { "kern.ostype", "kern.osrelease", "kern.version", "kern.hostname", "kern.boottime", "kern.maxproc",
        "kern.nprocs", "hw.machine", "hw.ncpu", "hw.activecpu", "hw.memsize", "hw.freemem", "hw.pagesize" };
    bool values_only = false;
    int first = 1;
    bool everything = false;
    for (; first < argc && argv[first][0] == '-'; ++first) {
        if (strcmp(argv[first], "-n") == 0)
            values_only = true;
        else if (strcmp(argv[first], "-a") == 0)
            everything = true;
    }
    auto show = [&](char const* name) {
        // A trailing "=value" sets it.
        char copy[128];
        strlcpy(copy, name, sizeof(copy));
        char* equals = strchr(copy, '=');
        if (equals) {
            *equals = '\0';
            if (sysctlbyname(copy, nullptr, nullptr, equals + 1, strlen(equals + 1)) < 0) {
                fprintf(stderr, "sysctl: %s: %s\n", copy, strerror(errno));
                return false;
            }
        }
        unsigned char value[256];
        size_t length = sizeof(value);
        if (sysctlbyname(copy, value, &length, nullptr, 0) < 0) {
            fprintf(stderr, "sysctl: unknown oid '%s'\n", copy);
            return false;
        }
        if (!values_only)
            printf("%s: ", copy);
        bool text = length > 0 && value[length - 1] == '\0';
        if (text) {
            printf("%s\n", value);
        } else if (length == 4) {
            int number;
            memcpy(&number, value, 4);
            printf("%d\n", number);
        } else if (length == 8) {
            long long number;
            memcpy(&number, value, 8);
            printf("%lld\n", number);
        } else {
            printf("(%zu bytes)\n", length);
        }
        return true;
    };
    int status = 0;
    if (everything || first == argc) {
        for (auto const* name : names)
            show(name);
        return 0;
    }
    for (int i = first; i < argc; ++i) {
        if (!show(argv[i]))
            status = 1;
    }
    return status;
}

// reboot / halt / poweroff / shutdown [-h | -r] [now]: asks process 1.
int tool_reboot(int argc, char** argv)
{
    char const* program = last_component(argv[0]);
    bool power_off = strcmp(program, "halt") == 0 || strcmp(program, "poweroff") == 0;
    if (strcmp(program, "shutdown") == 0) {
        power_off = true;
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "-r") == 0)
                power_off = false;
        }
    }
    if (geteuid() != 0) {
        fprintf(stderr, "%s: Operation not permitted\n", program);
        return 1;
    }
    // vlaunchd stops every job and writes everything out first: asked as
    // BSD's init is, SIGUSR2 to power off, SIGINT to restart.
    if (kill(1, power_off ? SIGUSR2 : SIGINT) == 0)
        return 0;
    sync();
    if (reboot(power_off ? RB_POWEROFF : RB_AUTOBOOT) < 0) {
        perror(program);
        return 1;
    }
    return 0;
}

struct Tool {
    char const* name;
    int (*main)(int, char**);
};

Tool const s_tools[] = {
    { "true", tool_true }, { "false", tool_false }, { "pwd", tool_pwd }, { "echo", tool_echo }, { "basename", tool_basename },
    { "dirname", tool_dirname }, { "readlink", tool_readlink }, { "realpath", tool_realpath }, { "mktemp", tool_mktemp },
    { "sync", tool_sync }, { "clear", tool_clear }, { "tty", tool_tty }, { "nproc", tool_nproc }, { "logname", tool_logname },
    { "stty", tool_stty }, { "sysctl", tool_sysctl }, { "reboot", tool_reboot }, { "halt", tool_reboot }, { "poweroff", tool_reboot },
    { "shutdown", tool_reboot },
};

}

int main(int argc, char** argv)
{
    char const* name = last_component(argv[0]);
    for (auto const& tool : s_tools) {
        if (strcmp(tool.name, name) == 0) {
            int status = tool.main(argc, argv);
            fflush(stdout);
            return status;
        }
    }
    fprintf(stderr, "%s: unknown tool\n", name);
    return 1;
}
