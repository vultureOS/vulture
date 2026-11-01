/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// The interactive shell, typed at through a pseudo-terminal the way a
// person would: line editing, history, ^R search, Tab completion, ^C,
// and job control (^Z, jobs, fg, background jobs, leaving with jobs
// suspended), with the messages zsh gives.

namespace {

int s_master = -1;
char s_transcript[65536];
size_t s_length = 0;
size_t s_position = 0; // Where the next expectation starts looking.

void type(char const* keys)
{
    (void)write(s_master, keys, strlen(keys));
}

void pause_milliseconds(long milliseconds)
{
    struct timespec duration { milliseconds / 1000, (milliseconds % 1000) * 1'000'000 };
    nanosleep(&duration, nullptr);
}

// Waits (up to five seconds) for `text` to appear after what the last
// expectation matched.
bool expect(char const* text)
{
    for (int waited = 0; waited < 5000;) {
        s_transcript[s_length] = '\0';
        if (char* found = strstr(s_transcript + s_position, text)) {
            s_position = static_cast<size_t>(found - s_transcript) + strlen(text);
            return true;
        }
        struct pollfd poller { s_master, POLLIN, 0 };
        int ready = poll(&poller, 1, 100);
        if (ready <= 0) {
            waited += 100;
            continue;
        }
        ssize_t got = read(s_master, s_transcript + s_length, sizeof(s_transcript) - 1 - s_length);
        if (got <= 0)
            break;
        s_length += static_cast<size_t>(got);
    }
    // Show what came instead, escapes made visible.
    fprintf(stderr, "interactive: never saw \"");
    for (char const* c = text; *c; ++c)
        fprintf(stderr, *c >= 0x20 && *c < 0x7f ? "%c" : "\\x%02x", static_cast<unsigned char>(*c));
    fprintf(stderr, "\"; after it came:\n  \"");
    for (size_t i = s_position; i < s_length; ++i)
        fprintf(stderr, s_transcript[i] >= 0x20 && s_transcript[i] < 0x7f ? "%c" : "\\x%02x", static_cast<unsigned char>(s_transcript[i]));
    fprintf(stderr, "\"\n");
    return false;
}

pid_t start_shell()
{
    mkdir("/tmp/vsh-home", 0755);
    unlink("/tmp/vsh-home/.vsh_history");
    s_master = posix_openpt(O_RDWR | O_NOCTTY);
    if (s_master < 0 || grantpt(s_master) < 0 || unlockpt(s_master) < 0)
        return -1;
    char slave_path[64];
    strncpy(slave_path, ptsname(s_master), sizeof(slave_path) - 1);
    slave_path[sizeof(slave_path) - 1] = '\0';
    pid_t child = fork();
    if (child == 0) {
        close(s_master);
        setsid();
        int slave = open(slave_path, O_RDWR);
        if (slave < 0)
            _exit(91);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2)
            close(slave);
        char const* argv[] = { "-vsh", nullptr };
        char const* envp[] = { "HOME=/tmp/vsh-home", "PS1=test> ", "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "TERM=xterm-256color", nullptr };
        execve("/bin/vsh", const_cast<char**>(argv), const_cast<char**>(envp));
        _exit(127);
    }
    return child;
}

}

int main()
{
    pid_t shell = start_shell();
    CHECK(shell > 0);
    if (shell <= 0)
        return finish();

    // A command and its output.
    CHECK(expect("test> "));
    type("echo hello\r");
    CHECK(expect("hello\r\ntest> "));

    // Up brings back the last line.
    type("\x1b[A");
    CHECK(expect("echo hello"));
    type("\r");
    CHECK(expect("hello\r\ntest> "));

    // Editing: ^A to the start, insert, ^E to the end.
    type("cho edited");
    type("\x01" "e" "\x05" "!\r");
    CHECK(expect("edited!\r\ntest> "));

    // ^R finds an older line by what it holds.
    type("echo first\r");
    CHECK(expect("first\r\ntest> "));
    type("echo second\r");
    CHECK(expect("second\r\ntest> "));
    type("\x12" "fir");
    CHECK(expect("bck-i-search: fir_"));
    type("\r");
    CHECK(expect("first\r\ntest> "));

    // Tab completes a command name.
    type("ech\t");
    CHECK(expect("echo "));
    type("completed\r");
    CHECK(expect("completed\r\ntest> "));

    // ^C abandons the line.
    type("never run\x03");
    CHECK(expect("^C\r\ntest> "));

    // A foreground job: suspended with ^Z, listed, brought back with fg.
    // (Typed as a person would, after cat has started; typed ahead, the
    // line would still reach cat, but unechoed, as with zsh.)
    type("cat\r");
    pause_milliseconds(200);
    type("abc\r");
    CHECK(expect("abc\r\nabc\r\n"));
    type("\x1a");
    CHECK(expect("vsh: suspended  cat\r\n"));
    CHECK(expect("test> "));
    type("jobs\r");
    CHECK(expect("[1]  + suspended  cat\r\n"));
    CHECK(expect("test> "));
    type("fg\r");
    CHECK(expect("[1]  + continued  cat\r\n"));
    pause_milliseconds(200);
    type("def\r");
    CHECK(expect("def\r\ndef\r\n"));
    type("\x04");
    CHECK(expect("test> "));

    // A background job reports itself when started and when done.
    type("sleep 0.1 &\r");
    CHECK(expect("[1] "));
    CHECK(expect("test> "));
    pause_milliseconds(400);
    type("true\r");
    CHECK(expect("[1]  + done       sleep 0.1\r\n"));
    CHECK(expect("test> "));

    // A crash is reported the way zsh does.
    type("sh -c 'kill -SEGV $$'\r");
    CHECK(expect("vsh: segmentation fault  sh -c 'kill -SEGV $$'\r\n"));
    CHECK(expect("test> "));

    // Leaving with a suspended job takes two tries.
    type("cat\r");
    pause_milliseconds(100);
    type("\x1a");
    CHECK(expect("vsh: suspended  cat\r\n"));
    CHECK(expect("test> "));
    type("exit\r");
    CHECK(expect("vsh: you have suspended jobs.\r\n"));
    CHECK(expect("test> "));
    type("exit 0\r");

    int status = -1;
    pid_t reaped = 0;
    for (int waited = 0; waited < 50 && (reaped = waitpid(shell, &status, WNOHANG)) == 0; ++waited)
        pause_milliseconds(100);
    CHECK(reaped == shell);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (reaped != shell || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fprintf(stderr, "interactive: the shell gave %d with status %#x\n", reaped, status);

    // The history file has what was typed.
    FILE* history = fopen("/tmp/vsh-home/.vsh_history", "r");
    CHECK(history != nullptr);
    if (history) {
        char line[256];
        int lines = 0;
        bool saw_first = false;
        while (fgets(line, sizeof(line), history)) {
            ++lines;
            saw_first = saw_first || strcmp(line, "echo first\n") == 0;
        }
        fclose(history);
        CHECK(saw_first);
        CHECK(lines >= 10);
    }
    close(s_master);
    return finish();
}
