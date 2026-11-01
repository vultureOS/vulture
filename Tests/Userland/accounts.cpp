/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "PseudoTerminal.h"
#include "Test.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Accounts, as a person at a terminal meets them: logging in (a wrong
// password first), the new home, who they are, sudo, su, and changing
// the password with passwd and logging in with the new one.

namespace {

char const* const s_environment[] = { "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "TERM=vt100", nullptr };

void expect_mode(char const* path, mode_t expected)
{
    struct stat information;
    CHECK(stat(path, &information) == 0);
    if ((information.st_mode & 07777) != expected)
        fprintf(stderr, "accounts: %s has mode %o, not %o\n", path, information.st_mode & 07777, expected);
    CHECK((information.st_mode & 07777) == expected);
}

// Logs in as vulture with `password` on a new terminal.
bool log_in(PseudoTerminal& terminal, char const* password)
{
    char const* argv[] = { "login", nullptr };
    if (!terminal.start("/usr/bin/login", argv, s_environment))
        return false;
    if (!terminal.expect("login: "))
        return false;
    terminal.type("vulture\r");
    if (!terminal.expect("Password:"))
        return false;
    terminal.type(password);
    terminal.type("\r");
    return true;
}

}

int main()
{
    // The modes macOS gives them.
    expect_mode("/etc/master.passwd", 0600);
    expect_mode("/usr/bin/login", 04555);
    expect_mode("/usr/bin/passwd", 04555);
    expect_mode("/usr/bin/su", 04755);
    expect_mode("/usr/bin/sudo", 04755);

    // Only root reads the hashes.
    CHECK(access("/etc/master.passwd", R_OK) == 0);
    // A clean slate for sudo's memory of passwords (it survives reboots).
    system("rm -rf /var/db/sudo/vulture");

    {
        PseudoTerminal terminal;
        char const* argv[] = { "login", nullptr };
        CHECK(terminal.start("/usr/bin/login", argv, s_environment));
        CHECK(terminal.expect("login: "));
        terminal.type("vulture\r");
        CHECK(terminal.expect("Password:"));
        // Not echoed.
        terminal.type("wrong\r");
        CHECK(terminal.expect("\r\nLogin incorrect\r\nlogin: "));
        CHECK(!terminal.saw("wrong"));
        terminal.type("vulture\r");
        CHECK(terminal.expect("Password:"));
        terminal.type("vulture\r");
        CHECK(terminal.expect("Welcome to vultureOS."));
        CHECK(terminal.expect("vulture@vulture ~ % "));

        // Who and where.
        terminal.type("id; pwd; echo $HOME $USER $LOGNAME $SHELL\r");
        CHECK(terminal.expect("uid=501(vulture) gid=20(staff) groups=20(staff),80(admin)\r\n"));
        CHECK(terminal.expect("/Users/vulture\r\n"));
        CHECK(terminal.expect("/Users/vulture vulture vulture /bin/vsh\r\n"));
        CHECK(terminal.expect("vulture@vulture ~ % "));
        // The home macOS makes.
        terminal.type("ls -1 ~; stat -f '%Su %Sg %Sp' ~ ~/Documents ~/Public\r");
        CHECK(terminal.expect("Desktop\r\nDocuments\r\nDownloads\r\nLibrary\r\nMovies\r\nMusic\r\nPictures\r\nPublic\r\n"));
        CHECK(terminal.expect("vulture staff drwxr-xr-x\r\nvulture staff drwx------\r\nvulture staff drwxr-xr-x\r\n"));
        CHECK(terminal.expect("vulture@vulture ~ % "));
        // The hashes are not ours to read.
        terminal.type("cat /etc/master.passwd\r");
        CHECK(terminal.expect("cat: /etc/master.passwd: Permission denied\r\n"));
        CHECK(terminal.expect("vulture@vulture ~ % "));

        // sudo: a wrong password, then the right one; then remembered.
        terminal.type("sudo id -un\r");
        CHECK(terminal.expect("Password:"));
        terminal.type("nope\r");
        CHECK(terminal.expect("Sorry, try again.\r\nPassword:"));
        terminal.type("vulture\r");
        CHECK(terminal.expect("root\r\n"));
        CHECK(terminal.expect("vulture@vulture ~ % "));
        terminal.type("sudo -u nobody id -u; echo $?\r");
        CHECK(terminal.expect("4294967294\r\n0\r\n"));
        CHECK(terminal.expect("vulture@vulture ~ % "));
        terminal.type("sudo -k; sudo true\r");
        CHECK(terminal.expect("Password:"));
        terminal.type("vulture\r");
        CHECK(terminal.expect("vulture@vulture ~ % "));

        // su: root has no password to give, as on macOS.
        terminal.type("su\r");
        CHECK(terminal.expect("Password:"));
        terminal.type("anything\r");
        CHECK(terminal.expect("su: Sorry\r\n"));
        CHECK(terminal.expect("vulture@vulture ~ % "));

        // passwd: the old one first, then the new one twice.
        terminal.type("passwd\r");
        CHECK(terminal.expect("Changing password for vulture.\r\nOld Password:"));
        terminal.type("vulture\r");
        CHECK(terminal.expect("New Password:"));
        terminal.type("swordfish\r");
        CHECK(terminal.expect("Retype New Password:"));
        terminal.type("swordfsh\r");
        CHECK(terminal.expect("Mismatch; try again, EOF to quit.\r\nNew Password:"));
        terminal.type("swordfish\r");
        CHECK(terminal.expect("Retype New Password:"));
        terminal.type("swordfish\r");
        CHECK(terminal.expect("vulture@vulture ~ % "));
        terminal.type("exit\r");
        int status = terminal.wait_for_exit();
        CHECK(status != -1 && WIFEXITED(status));
    }

    {
        // The old password no longer works; the new one does, and this
        // login sees the last.
        PseudoTerminal terminal;
        CHECK(log_in(terminal, "vulture"));
        CHECK(terminal.expect("Login incorrect\r\nlogin: "));
        terminal.type("vulture\r");
        CHECK(terminal.expect("Password:"));
        terminal.type("swordfish\r");
        CHECK(terminal.expect("Last login: "));
        CHECK(terminal.expect(" on ttys"));
        CHECK(terminal.expect("vulture@vulture ~ % "));
        // Root changes it back without the old one.
        terminal.type("exit\r");
        terminal.wait_for_exit();
    }
    {
        PseudoTerminal terminal;
        char const* argv[] = { "passwd", "vulture", nullptr };
        CHECK(terminal.start("/usr/bin/passwd", argv, s_environment));
        CHECK(terminal.expect("Changing password for vulture.\r\nNew Password:"));
        terminal.type("vulture\r");
        CHECK(terminal.expect("Retype New Password:"));
        terminal.type("vulture\r");
        int status = terminal.wait_for_exit();
        CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    {
        PseudoTerminal terminal;
        CHECK(log_in(terminal, "vulture"));
        CHECK(terminal.expect("vulture@vulture ~ % "));
        terminal.type("exit\r");
        terminal.wait_for_exit();
    }
    return finish();
}
