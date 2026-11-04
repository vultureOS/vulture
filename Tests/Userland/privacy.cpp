/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <Kernel/API/POSIX/privacy.h>
#include <LibBase/Types.h>
#include <LibCrypto/SHA2.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

// A person's private folders against installed code, with this program playing privacyd. "Installed"
// here is a changed copy of this program, which root has vouched for; run that way, it is the application
// asking.

namespace {

constexpr char const* private_file = "/Users/privacy-test/Documents/file";
constexpr char const* copy_path = "/tmp/privacy-test-app";
// Which is where /tmp really is.
constexpr char const* copy_real_path = "/private/tmp/privacy-test-app";

// Being the application. Exits with: 0 it could read it, 1 EPERM, 2 anything else.
int as_application(char const* what)
{
    int fd = open(what, O_RDONLY);
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    return errno == EPERM ? 1 : 2;
}

// Starts it, leaving out anything it says.
pid_t start(char const* program, char const* first, char const* second = nullptr)
{
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        char const* arguments[] = { program, first, second, nullptr };
        execv(program, const_cast<char* const*>(arguments));
        _exit(99);
    }
    return child;
}

int wait_for(pid_t child)
{
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 98;
}

bool make_installed_copy(char const* from)
{
    int in = open(from, O_RDONLY);
    int out = open(copy_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (in < 0 || out < 0)
        return false;
    Crypto::SHA256 hasher;
    static u8 buffer[64 * 1024];
    for (;;) {
        ssize_t got = read(in, buffer, sizeof(buffer));
        if (got <= 0)
            break;
        hasher.update({ buffer, static_cast<size_t>(got) });
        if (write(out, buffer, static_cast<size_t>(got)) != got)
            return false;
    }
    // A byte more, past where anything looks, and it is no longer what came with the system.
    u8 const extra = 0x5a;
    hasher.update({ &extra, 1 });
    if (write(out, &extra, 1) != 1)
        return false;
    close(in);
    close(out);
    auto const hash = hasher.digest();
    return sysctlbyname("security.code.trust", nullptr, nullptr, hash.data(), hash.size()) == 0;
}

// Reads the kernel's question from the device; false if none comes.
bool next_question(int device, vulture_privacy_request& request)
{
    pollfd poller { device, POLLIN, 0 };
    if (poll(&poller, 1, 5000) != 1)
        return false;
    return read(device, &request, sizeof(request)) == sizeof(request);
}

void answer(int device, vulture_privacy_request const& request, bool allowed)
{
    vulture_privacy_answer const reply { request.id, allowed ? 1u : 0u };
    CHECK(write(device, &reply, sizeof(reply)) == sizeof(reply));
}

}

int main(int argc, char** argv)
{
    // The two ways of being run.
    if (argc == 3 && strcmp(argv[1], "--try") == 0)
        return as_application(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--exec-cat") == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 1);
        dup2(null, 2);
        char const* arguments[] = { "/bin/cat", argv[2], nullptr };
        execv("/bin/cat", const_cast<char* const*>(arguments));
        _exit(99);
    }
    if (argc == 3 && strcmp(argv[1], "--twice") == 0) {
        int const first = as_application(argv[2]);
        int const second = as_application(argv[2]);
        return first == second ? first : 3;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
    (void)mkdir("/Users/privacy-test", 0755);
    (void)mkdir("/Users/privacy-test/Documents", 0755);
    (void)mkdir("/Users/privacy-test/Music", 0755);
    int fd = open(private_file, O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0);
    close(fd);
    fd = open("/Users/privacy-test/Music/file", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0);
    close(fd);

    CHECK(make_installed_copy(argv[0]));

    // Nobody to ask: no. (privacyd isn't running in this test yet.)
    CHECK(wait_for(start(copy_path, "--try", private_file)) == 1);

    int device = open("/dev/privacy", O_RDWR);
    CHECK(device >= 0);
    // Only root may be privacyd.
    pid_t other = fork();
    if (other == 0) {
        setuid(501);
        _exit(open("/dev/privacy", O_RDWR) < 0 && errno == EACCES ? 0 : 1);
    }
    CHECK(wait_for(other) == 0);

    // What isn't private isn't asked about.
    CHECK(wait_for(start(copy_path, "--try", "/Users/privacy-test/Music/file")) == 0);
    CHECK(wait_for(start(copy_path, "--try", "/etc/passwd")) == 0);

    // What came with the system isn't asked about either: not this program, not /bin/cat.
    fd = open(private_file, O_RDONLY);
    CHECK(fd >= 0);
    close(fd);
    pollfd quiet { device, POLLIN, 0 };
    CHECK(poll(&quiet, 1, 0) == 0);

    // Told no. It is asked once, by the application's right name, and that's the answer.
    pid_t application = start(copy_path, "--twice", private_file);
    vulture_privacy_request request {};
    CHECK(next_question(device, request));
    CHECK(strcmp(request.application, copy_real_path) == 0);
    CHECK(request.service == VULTURE_PRIVACY_DOCUMENTS);
    CHECK(request.pid == application);
    answer(device, request, false);
    CHECK(wait_for(application) == 1);
    CHECK(poll(&quiet, 1, 0) == 0);

    // Told yes.
    application = start(copy_path, "--twice", private_file);
    CHECK(next_question(device, request));
    answer(device, request, true);
    CHECK(wait_for(application) == 0);

    // The folder itself is as private as what's in it.
    application = start(copy_path, "--try", "/Users/privacy-test/Documents");
    CHECK(next_question(device, request));
    CHECK(request.service == VULTURE_PRIVACY_DOCUMENTS);
    answer(device, request, false);
    CHECK(wait_for(application) == 1);

    // Getting something that came with the system to do it is the same: it was started by the
    // application, and that's who is asking.
    application = start(copy_path, "--exec-cat", private_file);
    CHECK(next_question(device, request));
    CHECK(strcmp(request.application, copy_real_path) == 0);
    answer(device, request, false);
    CHECK(wait_for(application) == 1);

    // A symbolic link somewhere else doesn't make it somewhere else.
    (void)symlink(private_file, "/tmp/privacy-test-link");
    application = start(copy_path, "--try", "/tmp/privacy-test-link");
    CHECK(next_question(device, request));
    answer(device, request, false);
    CHECK(wait_for(application) == 1);
    unlink("/tmp/privacy-test-link");

    // Being told once by nobody isn't being told. With privacyd gone and back, it gets asked.
    close(device);
    CHECK(wait_for(start(copy_path, "--try", private_file)) == 1);
    device = open("/dev/privacy", O_RDWR);
    CHECK(device >= 0);
    close(device);

    // The kernel tells privacyd which it is.
    int third_party = -1;
    size_t size = sizeof(third_party);
    i32 me = getpid();
    CHECK(sysctlbyname("security.process.third-party", &third_party, &size, &me, sizeof(me)) == 0 && third_party == 0);

    unlink(copy_path);
    system("rm -rf /Users/privacy-test");
    return finish();
}
