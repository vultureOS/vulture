/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sandbox.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

// Trying to get out of a sandbox, as root, every way there is. Each attempt is in a child, as a sandbox
// is for good. A child exits with the number of the check that failed, or 0.

namespace {

constexpr char const* base = "/tmp/sandbox-test";

// Runs `body` in a sandbox of these rules, in a child, and checks it found everything as expected.
template<typename Function>
void in_sandbox(char const* rules, Function body)
{
    fflush(stdout);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        char* problem = nullptr;
        if (sandbox_init(rules, 0, &problem) != 0) {
            fprintf(stderr, "sandbox: %s\n", problem ? problem : "?");
            _exit(100);
        }
        body();
        fflush(stdout);
        _exit(finish());
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

bool denied(int result)
{
    return result == -1 && errno == EPERM;
}

void write_file(char const* path, char const* text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, text, strlen(text)) == static_cast<ssize_t>(strlen(text)));
    close(fd);
}

// What a sandboxed program needs to get as far as main: dyld loads it, and libSystem.
constexpr char const* to_run = "read /usr/lib\n";

void test_bad_profiles()
{
    pid_t child = fork();
    if (child == 0) {
        // A rule must be a whole path as the kernel would see it.
        CHECK(sandbox_init("read etc\n", 0, nullptr) == -1);
        CHECK(sandbox_init("read /etc/\n", 0, nullptr) == -1);
        CHECK(sandbox_init("read /etc/../tmp\n", 0, nullptr) == -1);
        CHECK(sandbox_init("read /etc//passwd\n", 0, nullptr) == -1);
        CHECK(sandbox_init("gibberish /etc\n", 0, nullptr) == -1);
        CHECK(sandbox_init("network sideways\n", 0, nullptr) == -1);
        CHECK(sandbox_init("no-such-profile", SANDBOX_NAMED, nullptr) == -1);
        CHECK(sandbox_init("../../../etc/passwd", SANDBOX_NAMED, nullptr) == -1);
        _exit(finish());
    }
    int status;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void test_files()
{
    in_sandbox("read /tmp/sandbox-test/readable\nwrite /tmp/sandbox-test/writable\n", [] {
        // As root, with nothing else in the way.
        CHECK(geteuid() == 0);
        CHECK(denied(open("/etc/master.passwd", O_RDONLY)));
        CHECK(denied(open("/etc/passwd", O_RDONLY)));
        CHECK(denied(open("/", O_RDONLY)));

        // What is allowed is.
        int fd = open("/tmp/sandbox-test/readable/file", O_RDONLY);
        CHECK(fd >= 0);
        close(fd);
        fd = open("/tmp/sandbox-test/writable/file", O_RDWR);
        CHECK(fd >= 0);
        close(fd);

        // A name that starts the same is something else.
        CHECK(denied(open("/tmp/sandbox-test/readable-too/file", O_RDONLY)));

        // Reading isn't writing.
        CHECK(denied(open("/tmp/sandbox-test/readable/file", O_WRONLY)));
        CHECK(denied(open("/tmp/sandbox-test/readable/file", O_RDWR)));
        CHECK(denied(open("/tmp/sandbox-test/readable/file", O_RDONLY | O_TRUNC)));
        CHECK(denied(open("/tmp/sandbox-test/readable/new", O_WRONLY | O_CREAT, 0644)));
        CHECK(denied(unlink("/tmp/sandbox-test/readable/file")));
        CHECK(denied(mkdir("/tmp/sandbox-test/readable/folder", 0755)));
        CHECK(denied(chmod("/tmp/sandbox-test/readable/file", 0777)));
        CHECK(denied(chown("/tmp/sandbox-test/readable/file", 501, 20)));
        CHECK(denied(truncate("/tmp/sandbox-test/readable/file", 0)));
        timespec times[2] = { { 0, 0 }, { 0, 0 } };
        CHECK(denied(utimensat(AT_FDCWD, "/tmp/sandbox-test/readable/file", times, 0)));

        // A way out through "..", from somewhere allowed.
        CHECK(denied(open("/tmp/sandbox-test/writable/../secret", O_RDONLY)));
        CHECK(denied(open("/tmp/sandbox-test/writable/../../../etc/passwd", O_RDONLY)));
        CHECK(chdir("/tmp/sandbox-test/writable") == 0);
        CHECK(denied(open("../secret", O_RDONLY)));

        // A way out through a link: made here, leading there.
        CHECK(symlink("/tmp/sandbox-test/secret", "/tmp/sandbox-test/writable/link") == 0);
        CHECK(denied(open("/tmp/sandbox-test/writable/link", O_RDONLY)));
        CHECK(denied(open("/tmp/sandbox-test/writable/link", O_WRONLY)));
        struct stat information;
        CHECK(denied(stat("/tmp/sandbox-test/writable/link", &information)));
        // And one that was already there.
        CHECK(denied(open("/tmp/sandbox-test/writable/old-link", O_RDONLY)));
        // A directory link, gone through.
        CHECK(symlink("/tmp/sandbox-test", "/tmp/sandbox-test/writable/up") == 0);
        CHECK(denied(open("/tmp/sandbox-test/writable/up/secret", O_RDONLY)));
        CHECK(denied(open("/tmp/sandbox-test/writable/up/new", O_WRONLY | O_CREAT, 0644)));
        CHECK(denied(mkdir("/tmp/sandbox-test/writable/up/new-folder", 0755)));

        // A second name for something outside, here where it could be opened.
        CHECK(denied(link("/tmp/sandbox-test/secret", "/tmp/sandbox-test/writable/hard-link")));
        // Moving it in, or moving what is here out.
        CHECK(denied(rename("/tmp/sandbox-test/secret", "/tmp/sandbox-test/writable/stolen")));
        CHECK(denied(rename("/tmp/sandbox-test/writable/file", "/tmp/sandbox-test/moved-out")));
        // Moving what holds the rules' paths around them.
        CHECK(denied(rename("/tmp/sandbox-test/writable", "/tmp/sandbox-test/elsewhere")));
        CHECK(denied(rename("/tmp/sandbox-test/readable", "/tmp/sandbox-test/writable/inside")));

        // The folders on the way can be looked at, not read; others can't.
        CHECK(stat("/tmp/sandbox-test", &information) == 0);
        CHECK(stat("/tmp", &information) == 0);
        CHECK(denied(open("/tmp/sandbox-test", O_RDONLY)));
        CHECK(denied(stat("/etc/passwd", &information)));
        CHECK(denied(stat("/tmp/sandbox-test/secret", &information)));
        CHECK(denied(access("/tmp/sandbox-test/secret", F_OK)));
        char target[64];
        CHECK(denied(static_cast<int>(readlink("/tmp/sandbox-test/outside-link", target, sizeof(target)))));

        // Within what is writable, all of it works.
        CHECK(mkdir("/tmp/sandbox-test/writable/folder", 0755) == 0);
        write_file("/tmp/sandbox-test/writable/folder/new", "new");
        CHECK(rename("/tmp/sandbox-test/writable/folder/new", "/tmp/sandbox-test/writable/renamed") == 0);
        CHECK(chmod("/tmp/sandbox-test/writable/renamed", 0600) == 0);
        CHECK(unlink("/tmp/sandbox-test/writable/renamed") == 0);
        CHECK(rmdir("/tmp/sandbox-test/writable/folder") == 0);
    });
}

void test_already_open()
{
    // What was open before is kept: that's how a process is handed just what it needs.
    int fd = open("/tmp/sandbox-test/secret", O_RDONLY);
    CHECK(fd >= 0);
    in_sandbox("read /nothing\n", [fd] {
        char buffer[8];
        CHECK(pread(fd, buffer, sizeof(buffer), 0) > 0);
        // But it can't be widened into more than it is.
        CHECK(denied(fchmod(fd, 0666)));
        CHECK(denied(fchown(fd, 501, 20)));
    });
    close(fd);
}

void test_programs()
{
    // To run something, it must be allowed; and whatever it runs, it runs inside too.
    static char rules[512];
    snprintf(rules, sizeof(rules), "%sexec /bin/cat\nexec /usr/bin/su\nexec /tmp/sandbox-test/script\nexec /bin/sh\nread /tmp/sandbox-test/readable\n", to_run);
    in_sandbox(rules, [] {
        auto run = [](char const* path, char const* argument) {
            fflush(stdout);
            pid_t child = fork();
            if (child == 0) {
                int null = open("/dev/null", O_WRONLY);
                if (null >= 0) {
                    dup2(null, 1);
                    dup2(null, 2);
                }
                char const* arguments[] = { path, argument, nullptr };
                execv(path, const_cast<char* const*>(arguments));
                _exit(errno == EPERM ? 111 : 112);
            }
            int status = 0;
            waitpid(child, &status, 0);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 113;
        };
        // Not on the list.
        CHECK(run("/bin/ls", "/") == 111);
        // On the list, and it can read what is allowed, but no more than this process can.
        CHECK(run("/bin/cat", "/tmp/sandbox-test/readable/file") == 0);
        CHECK(run("/bin/cat", "/tmp/sandbox-test/secret") == 1);
        // Set-ID: no way out by becoming someone else. (And it is root that runs it here.)
        CHECK(run("/usr/bin/su", "-") == 111);
    });
}

void test_network()
{
    in_sandbox("read /nothing\n", [] {
        CHECK(denied(socket(AF_INET, SOCK_STREAM, 0)));
        CHECK(denied(socket(AF_INET, SOCK_DGRAM, 0)));
        CHECK(denied(socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)));
    });
    in_sandbox("network outbound\n", [] {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fd >= 0);
        sockaddr_in address {};
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(1);
        // Allowed to try: nobody is there, which is a different answer.
        CHECK(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 && errno == ECONNREFUSED);
        close(fd);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        // But not to be a server.
        address.sin_port = htons(47001);
        CHECK(denied(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))));
        CHECK(denied(listen(fd, 1)));
        close(fd);
    });
    in_sandbox("network inbound\n", [] {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fd >= 0);
        sockaddr_in address {};
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(47002);
        CHECK(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(listen(fd, 1) == 0);
        close(fd);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        address.sin_port = htons(1);
        CHECK(denied(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))));
        close(fd);
    });
}

void test_narrowing()
{
    in_sandbox("read /tmp/sandbox-test/readable\n", [] {
        // Asking for more changes nothing.
        CHECK(sandbox_init("read /\nwrite /\nnetwork outbound\n", 0, nullptr) == 0);
        CHECK(denied(open("/etc/passwd", O_RDONLY)));
        CHECK(denied(open("/tmp/sandbox-test/readable/file", O_WRONLY)));
        CHECK(denied(socket(AF_INET, SOCK_STREAM, 0)));
        int fd = open("/tmp/sandbox-test/readable/file", O_RDONLY);
        CHECK(fd >= 0);
        close(fd);
        // Asking for less does.
        CHECK(sandbox_init("read /nothing\n", 0, nullptr) == 0);
        CHECK(denied(open("/tmp/sandbox-test/readable/file", O_RDONLY)));
    });
}

void test_children()
{
    in_sandbox("read /tmp/sandbox-test/readable\n", [] {
        pid_t child = fork();
        if (child == 0) {
            CHECK(denied(open("/etc/passwd", O_RDONLY)));
            int fd = open("/tmp/sandbox-test/readable/file", O_RDONLY);
            CHECK(fd >= 0);
            close(fd);
            _exit(finish());
        }
        int status;
        waitpid(child, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    });
}

void test_others()
{
    // A process outside, for it to be kept from.
    pid_t outside = fork();
    if (outside == 0) {
        pause();
        _exit(0);
    }
    in_sandbox("read /nothing\n", [outside] {
        // Others can't be signalled, and aren't even there to see.
        CHECK(kill(outside, SIGTERM) == -1 && errno == EPERM);
        CHECK(kill(1, SIGTERM) == -1 && errno == EPERM);
        // Something of its own can.
        pid_t mine = fork();
        if (mine == 0) {
            pause();
            _exit(0);
        }
        CHECK(kill(mine, SIGTERM) == 0);
        int status;
        waitpid(mine, &status, 0);
        CHECK(WIFSIGNALED(status));

        // It sees itself alone.
        size_t size = 0;
        CHECK(sysctlbyname("kern.proc.all", nullptr, &size, nullptr, 0) == 0);
        CHECK(size == sizeof(vulture_process_info));

        // Nothing about the machine can be changed.
        char const name[] = "other";
        CHECK(denied(sysctlbyname("kern.hostname", nullptr, nullptr, name, sizeof(name))));
        unsigned char hash[32] = {};
        CHECK(denied(sysctlbyname("security.code.trust", nullptr, nullptr, hash, sizeof(hash))));
        CHECK(denied(reboot(RB_POWEROFF)));
    });
    kill(outside, SIGKILL);
    int status;
    waitpid(outside, &status, 0);
}

void test_named_profile()
{
    pid_t child = fork();
    if (child == 0) {
        // Named, from the files that come with the system; and a variable filled in.
        char* problem = nullptr;
        CHECK(sandbox_init("viewer", SANDBOX_NAMED, &problem) == 0);
        int fd = open("/System/Library/Sandbox/Profiles/viewer.sb", O_RDONLY);
        CHECK(fd >= 0);
        close(fd);
        CHECK(denied(open("/etc/master.passwd", O_RDONLY)));
        // Root's home, here.
        CHECK(denied(open("/var/root/nothing", O_WRONLY | O_CREAT, 0644)));
        CHECK(denied(socket(AF_INET, SOCK_STREAM, 0)));
        _exit(finish());
    }
    int status;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    system("rm -rf /tmp/sandbox-test");
    CHECK(mkdir(base, 0755) == 0);
    CHECK(mkdir("/tmp/sandbox-test/readable", 0755) == 0);
    CHECK(mkdir("/tmp/sandbox-test/readable-too", 0755) == 0);
    CHECK(mkdir("/tmp/sandbox-test/writable", 0755) == 0);
    write_file("/tmp/sandbox-test/readable/file", "readable");
    write_file("/tmp/sandbox-test/readable-too/file", "other");
    write_file("/tmp/sandbox-test/writable/file", "writable");
    write_file("/tmp/sandbox-test/secret", "secret");
    CHECK(symlink("/tmp/sandbox-test/secret", "/tmp/sandbox-test/writable/old-link") == 0);
    CHECK(symlink("/tmp/sandbox-test/secret", "/tmp/sandbox-test/outside-link") == 0);

    test_bad_profiles();
    test_files();
    test_already_open();
    test_programs();
    test_network();
    test_narrowing();
    test_children();
    test_others();
    test_named_profile();
    system("rm -rf /tmp/sandbox-test");
    return finish();
}
