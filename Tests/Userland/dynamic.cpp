/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Dynamic linking: this program itself runs with libSystem.B.dylib (its
// constructors after the C library's setup, its destructors at exit), and
// copies of /bin/echo, patched to need a library or a function that is not
// there, fail as macOS's dyld fails them; DYLD_LIBRARY_PATH finds a
// library elsewhere, except for set-ID programs.

namespace {

char const* const s_directory = "/tmp/dyld-test";

// Set by a constructor, before main: the C library was ready for it.
bool s_constructor_saw_environment = false;

struct Setup {
    Setup() { s_constructor_saw_environment = getenv("PATH") != nullptr; }
} s_setup;

// With --child, its destructor (run by exit()) leaves a mark.
struct Marker {
    bool armed { false };
    ~Marker()
    {
        if (!armed)
            return;
        int fd = open("/tmp/dyld-test/destructed", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            (void)write(fd, "yes\n", 4);
            close(fd);
        }
    }
} s_marker;

bool read_all(char const* path, char* buffer, size_t size, size_t& length)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    length = 0;
    ssize_t got;
    while (length < size && (got = read(fd, buffer + length, size - length)) > 0)
        length += static_cast<size_t>(got);
    close(fd);
    return true;
}

bool write_all(char const* path, char const* buffer, size_t length, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return false;
    bool ok = write(fd, buffer, length) == static_cast<ssize_t>(length);
    close(fd);
    chmod(path, mode);
    return ok;
}

// A copy of `source` with the first `from` (in its string table) made `to`
// (as long).
bool patched_copy(char const* source, char const* destination, char const* from, char const* to)
{
    static char file[512 * 1024];
    size_t length = 0;
    if (!read_all(source, file, sizeof(file), length))
        return false;
    size_t size = strlen(from);
    for (size_t i = 0; i + size <= length; ++i) {
        // Whole names only: NUL before and after.
        if (memcmp(file + i, from, size) == 0 && i > 0 && file[i - 1] == '\0' && file[i + size] == '\0') {
            memcpy(file + i, to, size);
            return write_all(destination, file, length, 0755);
        }
    }
    return false;
}

struct Result {
    int status;
    char output[2048];
};

// Runs `path` (with `variable` added to the environment, if any); its
// standard output and error together.
Result run(char const* path, char const* variable = nullptr)
{
    Result result {};
    int fds[2];
    if (pipe(fds) < 0)
        return result;
    pid_t child = fork();
    if (child == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        if (variable)
            putenv(const_cast<char*>(variable));
        // (echo is one of the tools /bin/echo is: it goes by its name.)
        char const* argv[] = { "echo", "hello", nullptr };
        execv(path, const_cast<char**>(argv));
        _exit(127);
    }
    close(fds[1]);
    size_t length = 0;
    ssize_t got;
    while (length < sizeof(result.output) - 1 && (got = read(fds[0], result.output + length, sizeof(result.output) - 1 - length)) > 0)
        length += static_cast<size_t>(got);
    result.output[length] = '\0';
    close(fds[0]);
    waitpid(child, &result.status, 0);
    return result;
}

// "dyld[1234]: ..." with the pid left out.
bool says(Result const& result, char const* expected)
{
    char const* after_pid = strstr(result.output, "]: ");
    bool ok = strncmp(result.output, "dyld[", 5) == 0 && after_pid && strcmp(after_pid + 3, expected) == 0;
    if (!ok)
        fprintf(stderr, "dynamic: expected \"dyld[...]: %s\", got \"%s\"\n", expected, result.output);
    return ok;
}

}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--child") == 0) {
        s_marker.armed = true;
        return 0;
    }

    // This program: the C library was set up before our constructor ran.
    CHECK(s_constructor_saw_environment);

    system("rm -rf /tmp/dyld-test");
    CHECK(mkdir(s_directory, 0755) == 0);

    // Destructors run at exit.
    pid_t child = fork();
    if (child == 0) {
        char const* child_argv[] = { argv[0], "--child", nullptr };
        execv(argv[0], const_cast<char**>(child_argv));
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char mark[8] = {};
    size_t length = 0;
    CHECK(read_all("/tmp/dyld-test/destructed", mark, sizeof(mark) - 1, length) && strcmp(mark, "yes\n") == 0);

    // A library that is not there.
    CHECK(patched_copy("/bin/echo", "/tmp/dyld-test/needs-missing", "libSystem.B.dylib", "libSystem.X.dylib"));
    auto missing = run("/tmp/dyld-test/needs-missing");
    CHECK(WIFSIGNALED(missing.status) && WTERMSIG(missing.status) == SIGABRT);
    CHECK(says(missing,
        "Library not loaded: libSystem.X.dylib\n"
        "  Referenced from: /tmp/dyld-test/needs-missing\n"
        "  Reason: tried: '/usr/lib/libSystem.X.dylib' (no such file), '/usr/local/lib/libSystem.X.dylib' (no such file)\n"));

    // DYLD_LIBRARY_PATH finds it elsewhere.
    CHECK(mkdir("/tmp/dyld-test/lib", 0755) == 0);
    static char library[512 * 1024];
    CHECK(read_all("/usr/lib/libSystem.B.dylib", library, sizeof(library), length));
    CHECK(write_all("/tmp/dyld-test/lib/libSystem.X.dylib", library, length, 0755));
    auto found = run("/tmp/dyld-test/needs-missing", "DYLD_LIBRARY_PATH=/tmp/dyld-test/lib");
    CHECK(WIFEXITED(found.status) && WEXITSTATUS(found.status) == 0);
    CHECK(strcmp(found.output, "hello\n") == 0);
    if (strcmp(found.output, "hello\n") != 0)
        fprintf(stderr, "dynamic: with DYLD_LIBRARY_PATH: status %#x, \"%s\"\n", found.status, found.output);

    // Not for a set-user-ID program (running as someone else than its
    // caller): only the system's places.
    CHECK(chown("/tmp/dyld-test/needs-missing", 501, 20) == 0);
    CHECK(chmod("/tmp/dyld-test/needs-missing", 04755) == 0);
    auto secure = run("/tmp/dyld-test/needs-missing", "DYLD_LIBRARY_PATH=/tmp/dyld-test/lib");
    CHECK(WIFSIGNALED(secure.status) && WTERMSIG(secure.status) == SIGABRT);
    CHECK(strstr(secure.output, "Library not loaded: libSystem.X.dylib") != nullptr);
    CHECK(strstr(secure.output, "/tmp/dyld-test/lib/") == nullptr);

    // A function that is not there.
    CHECK(patched_copy("/bin/echo", "/tmp/dyld-test/needs-symbol", "putchar", "putchaz"));
    auto symbol = run("/tmp/dyld-test/needs-symbol");
    CHECK(WIFSIGNALED(symbol.status) && WTERMSIG(symbol.status) == SIGABRT);
    CHECK(says(symbol,
        "Symbol not found: putchaz\n"
        "  Referenced from: /tmp/dyld-test/needs-symbol\n"
        "  Expected in: flat namespace\n"));

    // Not a library at all.
    CHECK(write_all("/tmp/dyld-test/lib/libSystem.X.dylib", "not ELF at all\n", 15, 0755));
    CHECK(chmod("/tmp/dyld-test/needs-missing", 0755) == 0);
    auto garbage = run("/tmp/dyld-test/needs-missing", "DYLD_LIBRARY_PATH=/tmp/dyld-test/lib");
    CHECK(strstr(garbage.output, "'/tmp/dyld-test/lib/libSystem.X.dylib' (not an ELF file)") != nullptr);

    system("rm -rf /tmp/dyld-test");
    return finish();
}
