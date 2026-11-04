/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "NativeMachine.h"
#include "Test.h"
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// The command-line tools: each case runs with /bin/sh -c, and its output
// and exit status must be exactly what the macOS tool gives.

namespace {

struct Case {
    char const* script;
    char const* output;
    int status;
};

Case const s_cases[] = {
    // Text.
    { "printf 'b\\na\\nc\\n' | sort", "a\nb\nc\n", 0 },
    { "printf '3\\n10\\n2\\n' | sort -n", "2\n3\n10\n", 0 },
    { "printf '3\\n10\\n2\\n' | sort -rn | head -1", "10\n", 0 },
    { "printf 'b 2\\na 1\\nc 3\\n' | sort -k2 -n", "a 1\nb 2\nc 3\n", 0 },
    { "printf 'x:3\\ny:1\\n' | sort -t: -k2", "y:1\nx:3\n", 0 },
    { "printf 'b\\na\\nb\\n' | sort -u", "a\nb\n", 0 },
    { "printf 'a\\na\\nb\\na\\n' | uniq -c", "   2 a\n   1 b\n   1 a\n", 0 },
    { "printf 'a\\na\\nb\\n' | uniq -d", "a\n", 0 },
    { "printf 'one two\\nthree\\n' | wc", "       2       3      14\n", 0 },
    { "printf 'a\\nb\\nc\\n' | wc -l", "       3\n", 0 },
    { "seq 3", "1\n2\n3\n", 0 },
    { "seq -s, 1 2 7", "1,3,5,7\n", 0 },
    { "seq -w 8 10", "08\n09\n10\n", 0 },
    { "seq 10 | head -3 | tail -1", "3\n", 0 },
    { "seq 10 | tail -2", "9\n10\n", 0 },
    { "seq 5 | tail -n +4", "4\n5\n", 0 },
    { "printf abcdef | head -c 3", "abc", 0 },
    { "echo hello | tr a-z A-Z", "HELLO\n", 0 },
    { "echo 'a  b   c' | tr -s ' '", "a b c\n", 0 },
    { "echo hello | tr -d l", "heo\n", 0 },
    { "echo 'Mixed 123' | tr '[:lower:]' '[:upper:]'", "MIXED 123\n", 0 },
    { "echo 'a:b:c' | cut -d: -f2", "b\n", 0 },
    { "echo 'a:b:c:d' | cut -d: -f1,3-", "a:c:d\n", 0 },
    { "echo abcdef | cut -c2-4", "bcd\n", 0 },
    { "echo hello world | sed 's/o/0/g'", "hell0 w0rld\n", 0 },
    { "echo hello world | sed 's/o/0/2'", "hello w0rld\n", 0 },
    { "printf 'a\\nb\\nc\\n' | sed -n 2p", "b\n", 0 },
    { "printf 'a\\nb\\nc\\n' | sed 2d", "a\nc\n", 0 },
    { "printf 'a\\nb\\nc\\nd\\n' | sed -n '2,3p'", "b\nc\n", 0 },
    { "printf 'x\\nstart\\ny\\nend\\nz\\n' | sed '/start/,/end/d'", "x\nz\n", 0 },
    { "echo 'key=value' | sed -E 's/([a-z]+)=([a-z]+)/\\2=\\1/'", "value=key\n", 0 },
    { "echo 'key=value' | sed 's/\\(.*\\)=\\(.*\\)/[&]/'", "[key=value]\n", 0 },
    { "printf 'a\\nb\\n' | sed -e '1i\\\ntop' -e '$a\\\nbottom'", "top\na\nb\nbottom\n", 0 },
    { "echo abc | sed 'y/abc/xyz/'", "xyz\n", 0 },
    { "printf 'a\\nb\\nc\\n' | sed '2q'", "a\nb\n", 0 },
    { "printf 'x1\\ny2\\nx3\\n' | grep x", "x1\nx3\n", 0 },
    { "printf 'x1\\ny2\\nx3\\n' | grep -c x", "2\n", 0 },
    { "printf 'x1\\ny2\\nx3\\n' | grep -v x", "y2\n", 0 },
    { "printf 'x1\\ny2\\nx3\\n' | grep -n 3", "3:x3\n", 0 },
    { "printf 'Apple\\nbanana\\n' | grep -i apple", "Apple\n", 0 },
    { "printf 'ab12\\nab\\n12\\n' | grep -E '^[a-z]+[0-9]+$'", "ab12\n", 0 },
    { "printf 'aaa\\nab\\n' | grep 'a\\{3\\}'", "aaa\n", 0 },
    { "printf 'cat\\ndog\\nbird\\n' | grep -E 'cat|bird'", "cat\nbird\n", 0 },
    { "printf 'a.b\\naxb\\n' | grep -F a.b", "a.b\n", 0 },
    { "printf 'one word\\nwordy\\n' | grep -w word", "one word\n", 0 },
    { "echo abcabc | grep -o b", "b\nb\n", 0 },
    { "echo nothing | grep missing; echo $?", "1\n", 0 },
    { "echo hi | grep -q hi && echo quiet", "quiet\n", 0 },
    { "printf '%5s|%-3d|%x|%o|%c\\n' ab 7 255 8 z", "   ab|7  |ff|10|z\n", 0 },
    { "printf '%s-%s\\n' a b c d", "a-b\nc-d\n", 0 },
    { "printf '%.2f %03d %%\\n' 3.14159 7", "3.14 007 %\n", 0 },
    { "yes | head -2", "y\ny\n", 0 },
    { "yes no | head -1", "no\n", 0 },
    { "printf AB | hexdump -C", "00000000  41 42                                             |AB|\n00000002\n", 0 },
    { "echo hi | tee /tmp/tools-tee | cat; cat /tmp/tools-tee; rm /tmp/tools-tee", "hi\nhi\n", 0 },
    { "echo one two three | xargs -n1", "one\ntwo\nthree\n", 0 },
    { "printf '1\\n2\\n' | xargs echo sum", "sum 1 2\n", 0 },
    { "printf 'a\\nb\\n' | xargs -I{} echo '<{}>'", "<a>\n<b>\n", 0 },

    // Files.
    { "rm -rf /tmp/t && mkdir -p /tmp/t/a/b && touch /tmp/t/a/b/f && find /tmp/t -type f", "/tmp/t/a/b/f\n", 0 },
    { "find /tmp/t -name 'b' -type d", "/tmp/t/a/b\n", 0 },
    { "find /tmp/t -maxdepth 1", "/tmp/t\n/tmp/t/a\n", 0 },
    { "find /tmp/t -name f -exec echo found {} ';'", "found /tmp/t/a/b/f\n", 0 },
    { "ls /tmp/t/a", "b\n", 0 },
    { "ls -d /tmp/t/a", "/tmp/t/a\n", 0 },
    { "echo x > /tmp/t/x && chmod 640 /tmp/t/x && stat -f %Sp /tmp/t/x", "-rw-r-----\n", 0 },
    { "chmod u+x,o+r /tmp/t/x && stat -f %Sp /tmp/t/x", "-rwxr--r--\n", 0 },
    { "ls -l /tmp/t/x | cut -c1-10", "-rwxr--r--\n", 0 },
    { "stat -f '%z %HT' /tmp/t/x", "2 Regular File\n", 0 },
    { "cp /tmp/t/x /tmp/t/y && cat /tmp/t/y", "x\n", 0 },
    { "mv /tmp/t/y /tmp/t/z && ls /tmp/t", "a\nx\nz\n", 0 },
    { "cp -R /tmp/t/a /tmp/t/c && find /tmp/t/c | sort", "/tmp/t/c\n/tmp/t/c/b\n/tmp/t/c/b/f\n", 0 },
    { "ln -s x /tmp/t/link && readlink /tmp/t/link && cat /tmp/t/link", "x\nx\n", 0 },
    { "ln /tmp/t/x /tmp/t/hard && stat -f %l /tmp/t/x", "2\n", 0 },
    { "rm /tmp/t/link /tmp/t/hard && rmdir /tmp/t/c/b 2>/dev/null; echo $?", "1\n", 0 },
    { "rm -r /tmp/t/c && ls /tmp/t", "a\nx\nz\n", 0 },
    { "touch /tmp/t/new && test -f /tmp/t/new && [ -s /tmp/t/new ] || echo empty", "empty\n", 0 },
    { "echo 12345 > /tmp/t/n; du -k /tmp/t/n | cut -f2", "/tmp/t/n\n", 0 },
    { "rm -rf /tmp/t; ls /tmp/t 2>/dev/null; echo $?", "1\n", 0 },
    { "basename /a/b/c.txt .txt; basename /a/b/; dirname /a/b/c; dirname c", "c\nb\n/a/b\n.\n", 0 },
    { "realpath /usr/../bin", "/bin\n", 0 },
    { "d=$(mktemp -d) && [ -d \"$d\" ] && rmdir \"$d\" && echo made", "made\n", 0 },
    { "ls -l /System/Library | grep Kernel | wc -l", "       1\n", 0 },
    { "ls /System/Library/Kernels", "vulture\n", 0 },

    // The system.
    { "uname", "vultureOS\n", 0 },
    { "uname -m", NATIVE_MACHINE "\n", 0 },
    { "id -u; id -un; whoami; id -g; groups", "0\nroot\nroot\n0\nwheel\n", 0 },
    { "id", "uid=0(root) gid=0(wheel) groups=0(wheel)\n", 0 },
    { "env -i A=1 B=2 /usr/bin/env", "A=1\nB=2\n", 0 },
    { "env -i /usr/bin/which ls cat", "/bin/ls\n/bin/cat\n", 0 },
    { "which nosuch; echo $?", "1\n", 0 },
    { "true; echo $?; false; echo $?; /usr/bin/true; echo $?", "0\n1\n0\n", 0 },
    { "/bin/test 1 -lt 2 && /bin/[ a = a ] && ! /bin/[ -d /nonexistent ] && echo ok", "ok\n", 0 },
    { "/bin/echo -n a; /bin/echo b c", "ab c\n", 0 },
    { "cd /usr/bin && /bin/pwd", "/usr/bin\n", 0 },
    { "date -u -r 0 '+%Y-%m-%d %H:%M:%S'", "1970-01-01 00:00:00\n", 0 },
    { "date -u -r 86399 '+%a %b %e %T'", "Thu Jan  1 23:59:59\n", 0 },
    { "ps -A | head -1", "  PID TTY           TIME CMD\n", 0 },
    { "ps -p 1 | tail -1 | grep -c ' vlaunchd$'", "1\n", 0 },
    { "ps -p $$ | tail -1 | grep -c sh", "1\n", 0 },
    { "df | head -1", "Filesystem     512-blocks       Used  Available Capacity  Mounted on\n", 0 },
    { "df | grep -c ' /System/Volumes/Data$'", "1\n", 0 },
    { "sleep 0.01 && echo slept", "slept\n", 0 },
    { "sleep 5 & kill $!; wait $!; echo $?", "143\n", 0 },
    { "kill -l | head -1 | cut -d' ' -f1-3; kill -l 9; /bin/kill -l 143", "HUP INT QUIT\nKILL\nTERM\n", 0 },
    { "sysctl -n kern.ostype", "vultureOS\n", 0 },
    { "nproc | grep -c '^[0-9]'", "1\n", 0 },
    { "hostname | wc -l", "       1\n", 0 },
    { "uptime | grep -c ' up '", "1\n", 0 },
    { "time true 2>&1 | grep -c real", "1\n", 0 },
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
    char output[8192];
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
        fprintf(stderr, "tools: script: %s\n  expected status %d, output \"%s\"\n  got status %d, output \"%s\"\n", test.script, test.status, test.output,
            WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status), output);
    return ok;
}

}

int main()
{
    for (auto const& test : s_cases)
        CHECK(run(test));
    return finish();
}
