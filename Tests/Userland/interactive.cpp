/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "PseudoTerminal.h"
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

PseudoTerminal s_terminal;

void type(char const* keys) { s_terminal.type(keys); }
bool expect(char const* text) { return s_terminal.expect(text); }
void pause_milliseconds(long milliseconds) { PseudoTerminal::pause_milliseconds(milliseconds); }

pid_t start_shell()
{
    mkdir("/tmp/vsh-home", 0755);
    unlink("/tmp/vsh-home/.vsh_history");
    char const* argv[] = { "-vsh", nullptr };
    char const* envp[] = { "HOME=/tmp/vsh-home", "PS1=test> ", "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "TERM=xterm-256color", nullptr };
    if (!s_terminal.start("/bin/vsh", argv, envp))
        return -1;
    return s_terminal.child();
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

    int status = s_terminal.wait_for_exit();
    CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fprintf(stderr, "interactive: the shell ended with status %#x\n", status);

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
    return finish();
}
