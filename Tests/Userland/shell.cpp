/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// The shell language: each script runs with /bin/sh -c, and its output
// and exit status must be exactly as POSIX says.

namespace {

struct Case {
    char const* script;
    char const* output;
    int status;
};

Case const s_cases[] = {
    { "echo hello world", "hello world\n", 0 },
    { "echo -n no newline", "no newline", 0 },
    { "a=1; b=$a$a; echo $b ${b}x \"$b\"", "11 11x 11\n", 0 },
    { "echo 'single $quotes' \"double $HOME_UNSET\"x", "single $quotes double x\n", 0 },
    { "echo \\$escaped \\\\ back", "$escaped \\ back\n", 0 },
    { "x='a  b'; for w in $x; do echo \"[$w]\"; done", "[a]\n[b]\n", 0 },
    { "x='a  b'; for w in \"$x\"; do echo \"[$w]\"; done", "[a  b]\n", 0 },
    { "set -- one 'two three'; for a in \"$@\"; do echo \"<$a>\"; done; echo $#", "<one>\n<two three>\n2\n", 0 },
    { "echo $((1 + 2 * 3)) $(( (1+2)*3 )) $((10 % 4)) $((2**10)) $((7 > 3 && 2 < 1))", "7 9 2 1024 0\n", 0 },
    { "i=0; while [ $i -lt 3 ]; do i=$((i+1)); echo $i; done", "1\n2\n3\n", 0 },
    { "for i in 1 2 3 4 5; do if [ $i = 2 ]; then continue; fi; if [ $i = 4 ]; then break; fi; echo $i; done", "1\n3\n", 0 },
    { "until false; do echo once; break; done", "once\n", 0 },
    { "if false; then echo a; elif true; then echo b; else echo c; fi", "b\n", 0 },
    { "case hello.txt in *.c) echo c;; *.txt|*.md) echo text;; *) echo other;; esac", "text\n", 0 },
    { "f() { echo \"f:$1:$#\"; return 3; }; f arg x; echo $?", "f:arg:2\n3\n", 0 },
    { "echo $(echo nested $(echo deeper)) `echo back`", "nested deeper back\n", 0 },
    { "x=$(printf 'a\\n\\n'); echo \"[$x]\"", "[a]\n", 0 },
    { "true && echo yes; false && echo no; false || echo fallback; ! true; echo $?", "yes\nfallback\n1\n", 0 },
    { "echo one | { read a; echo got $a; }", "got one\n", 0 },
    { "(exit 7); echo $?", "7\n", 0 },
    { "echo out > /tmp/shell-test; echo more >> /tmp/shell-test; while read line; do echo \"<$line>\"; done < /tmp/shell-test", "<out>\n<more>\n", 0 },
    { "cat_here() { while read l; do echo \"$l\"; done; }; cat_here <<EOF\nline $((1+1))\n  'kept'\nEOF", "line 2\n'kept'\n", 0 },
    { "while read l; do echo \"$l\"; done <<'EOF'\nliteral $HOME\nEOF", "literal $HOME\n", 0 },
    { "read a b <<< 'x y z'; echo \"$a|$b\"", "x|y z\n", 0 },
    { "echo error 1>&2 2>/dev/null; echo kept", "kept\n", 0 },
    { "v=; echo ${v:-default} ${v-set} ${u=assigned} $u ${#u}", "default assigned assigned 8\n", 0 },
    { "p=/usr/lib/libc.a; echo ${p##*/} ${p%/*} ${p#/} ${p%.a}", "libc.a /usr/lib usr/lib/libc.a /usr/lib/libc\n", 0 },
    { "s=banana; echo ${s/an/AN} ${s//an/AN} ${s:1:3}", "bANana bANANa ana\n", 0 },
    { "[ -d /usr ] && [ ! -f /usr ] && [ -n x ] && [ -z '' ] && [ 3 -ge 3 ] && test abc != abd && echo tests", "tests\n", 0 },
    { "exit 42", "", 42 },
    { "set -e; false; echo not reached", "", 1 },
    { "echo /usr/tests/hell*", "/usr/tests/hello\n", 0 },
    { "echo '/usr/tests/hell*'", "/usr/tests/hell*\n", 0 },
    { "echo /no/match/*.x", "/no/match/*.x\n", 0 },
    { "alias say='echo said'; say it", "said it\n", 0 },
    { "x=outer; f() { x=inner; }; f; echo $x", "inner\n", 0 },
    { "sleep_and() { echo bg; }; sleep_and & wait; echo after", "bg\nafter\n", 0 },
    { "export E=exported; /bin/sh -c 'echo $E'", "exported\n", 0 },
    { "echo $((0x10 + 010))", "24\n", 0 },
    { "nosuchcommand 2>/dev/null; echo $?", "127\n", 0 },
    { "type cd", "cd is a shell builtin\n", 0 },
    { "echo a; # a comment\necho b", "a\nb\n", 0 },
    { "x=5; x=$((x * 2)); : $((x += 1)); echo $x", "11\n", 0 },
    { "set -- a b c; shift; echo $1 $#", "b 2\n", 0 },
};

bool run(Case const& test)
{
    int fds[2];
    if (pipe(fds) < 0)
        return false;
    pid_t child = fork();
    if (child == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", test.script, nullptr);
        _exit(126);
    }
    close(fds[1]);
    char output[4096];
    size_t length = 0;
    ssize_t got;
    while ((got = read(fds[0], output + length, sizeof(output) - 1 - length)) > 0)
        length += static_cast<size_t>(got);
    output[length] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(child, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == test.status && strcmp(output, test.output) == 0;
    if (!ok)
        fprintf(stderr, "shell: script: %s\n  expected status %d, output \"%s\"\n  got status %d, output \"%s\"\n", test.script, test.status, test.output,
            WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status), output);
    return ok;
}

}

int main()
{
    for (auto const& test : s_cases)
        CHECK(run(test));
    unlink("/tmp/shell-test");
    return finish();
}
