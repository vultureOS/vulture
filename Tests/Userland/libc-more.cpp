/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <glob.h>
#include <grp.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// The parts of LibC that the shell and the command-line tools lean on.

namespace {

void test_getopt()
{
    char* argv[] = { const_cast<char*>("tool"), const_cast<char*>("-ab"), const_cast<char*>("-o"), const_cast<char*>("out"),
        const_cast<char*>("-vfile"), const_cast<char*>("--"), const_cast<char*>("-x"), nullptr };
    int argc = 7;
    optind = 1;
    char seen[16] = {};
    size_t count = 0;
    char const* output = nullptr;
    char const* value = nullptr;
    for (int option; (option = getopt(argc, argv, "abo:v:")) != -1;) {
        seen[count++] = static_cast<char>(option);
        if (option == 'o')
            output = optarg;
        if (option == 'v')
            value = optarg;
    }
    CHECK(strcmp(seen, "abov") == 0);
    CHECK(output && strcmp(output, "out") == 0);
    CHECK(value && strcmp(value, "file") == 0);
    CHECK(optind == 6 && strcmp(argv[optind], "-x") == 0);

    char* long_argv[] = { const_cast<char*>("tool"), const_cast<char*>("--name=vulture"), const_cast<char*>("--verbose"), nullptr };
    option const options[] = { { "name", required_argument, nullptr, 'n' }, { "verbose", no_argument, nullptr, 'v' }, { nullptr, 0, nullptr, 0 } };
    optind = 1;
    CHECK(getopt_long(3, long_argv, "", options, nullptr) == 'n' && strcmp(optarg, "vulture") == 0);
    CHECK(getopt_long(3, long_argv, "", options, nullptr) == 'v');
    CHECK(getopt_long(3, long_argv, "", options, nullptr) == -1);
}

void test_patterns()
{
    CHECK(fnmatch("*.txt", "notes.txt", 0) == 0);
    CHECK(fnmatch("*.txt", ".hidden.txt", FNM_PERIOD) == FNM_NOMATCH);
    CHECK(fnmatch("file-[0-9]?", "file-3a", 0) == 0);
    CHECK(fnmatch("file-[!0-9]", "file-3", 0) == FNM_NOMATCH);
    CHECK(fnmatch("a/*", "a/b/c", FNM_PATHNAME) == FNM_NOMATCH);
    CHECK(fnmatch("\\*", "*", 0) == 0);

    mkdir("/tmp/globs", 0755);
    char const* names[] = { "/tmp/globs/one.c", "/tmp/globs/two.c", "/tmp/globs/three.h", "/tmp/globs/.secret.c" };
    for (auto const* name : names)
        close(open(name, O_WRONLY | O_CREAT, 0644));
    glob_t found {};
    CHECK(glob("/tmp/globs/*.c", 0, nullptr, &found) == 0);
    CHECK(found.gl_pathc == 2 && strcmp(found.gl_pathv[0], "/tmp/globs/one.c") == 0 && strcmp(found.gl_pathv[1], "/tmp/globs/two.c") == 0);
    globfree(&found);
    CHECK(glob("/tmp/g*/t*", 0, nullptr, &found) == 0 && found.gl_pathc == 2);
    globfree(&found);
    CHECK(glob("/tmp/globs/*.zip", 0, nullptr, &found) == GLOB_NOMATCH);
    CHECK(glob("/tmp/globs/*.zip", GLOB_NOCHECK, nullptr, &found) == 0 && found.gl_pathc == 1);
    globfree(&found);
    for (auto const* name : names)
        unlink(name);
    rmdir("/tmp/globs");
}

void test_time()
{
    time_t moment = 1'700'000'000; // 2023-11-14 22:13:20 UTC, a Tuesday.
    struct tm broken {};
    gmtime_r(&moment, &broken);
    CHECK(broken.tm_year == 123 && broken.tm_mon == 10 && broken.tm_mday == 14);
    CHECK(broken.tm_hour == 22 && broken.tm_min == 13 && broken.tm_sec == 20 && broken.tm_wday == 2 && broken.tm_yday == 317);
    char text[64];
    CHECK(strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %a %b %j", &broken) > 0);
    CHECK(strcmp(text, "2023-11-14 22:13:20 Tue Nov 318") == 0);
    CHECK(timegm(&broken) == moment);
    // Leap day, and normalizing out-of-range fields.
    struct tm leap {};
    leap.tm_year = 124;
    leap.tm_mon = 1;
    leap.tm_mday = 30; // February 30th is March 1st.
    time_t normalized = timegm(&leap);
    CHECK(leap.tm_mon == 2 && leap.tm_mday == 1 && normalized == 1709251200);
    setenv("TZ", "UTC+5:30", 1);
    struct tm local {};
    localtime_r(&moment, &local);
    CHECK(local.tm_hour == 3 && local.tm_min == 43 && local.tm_mday == 15 && local.tm_gmtoff == 19800);
    unsetenv("TZ");
    struct timeval now {};
    CHECK(gettimeofday(&now, nullptr) == 0 && now.tv_sec > 1'700'000'000);
    CHECK(time(nullptr) - now.tv_sec <= 1);
}

void test_environment_and_paths()
{
    CHECK(setenv("VULTURE_TEST", "one", 1) == 0 && strcmp(getenv("VULTURE_TEST"), "one") == 0);
    CHECK(setenv("VULTURE_TEST", "two", 0) == 0 && strcmp(getenv("VULTURE_TEST"), "one") == 0);
    CHECK(setenv("VULTURE_TEST", "three", 1) == 0 && strcmp(getenv("VULTURE_TEST"), "three") == 0);
    CHECK(unsetenv("VULTURE_TEST") == 0 && getenv("VULTURE_TEST") == nullptr);

    char resolved[PATH_MAX];
    CHECK(realpath("/usr/../usr/./tests/", resolved) && strcmp(resolved, "/usr/tests") == 0);
    CHECK(symlink("/usr/tests", "/tmp/tests-link") == 0);
    CHECK(realpath("/tmp/tests-link/../tests", resolved) && strcmp(resolved, "/usr/tests") == 0);
    unlink("/tmp/tests-link");
    CHECK(realpath("/no/such/place", resolved) == nullptr && errno == ENOENT);

    char path_a[] = "/usr/lib/libc.a", path_b[] = "/usr/lib/libc.a";
    CHECK(strcmp(basename(path_a), "libc.a") == 0 && strcmp(dirname(path_b), "/usr/lib") == 0);
    char root[] = "/", plain[] = "file";
    CHECK(strcmp(dirname(root), "/") == 0 && strcmp(dirname(plain), ".") == 0);

    char template_path[] = "/tmp/test.XXXXXX";
    int fd = mkstemp(template_path);
    CHECK(fd >= 0 && strncmp(template_path, "/tmp/test.", 10) == 0 && strcmp(template_path, "/tmp/test.XXXXXX") != 0);
    FILE* file = fdopen(fd, "w+");
    CHECK(file != nullptr);
    fprintf(file, "alpha 42 3.5\nsecond line\n");
    rewind(file);
    char word[16];
    int number = 0;
    float real = 0;
    CHECK(fscanf(file, "%15s %d %f", word, &number, &real) == 3 && strcmp(word, "alpha") == 0 && number == 42 && real == 3.5f);
    char* line = nullptr;
    size_t capacity = 0;
    CHECK(getline(&line, &capacity, file) == 1); // The rest of the first line.
    CHECK(getline(&line, &capacity, file) == 12 && strcmp(line, "second line\n") == 0);
    CHECK(getline(&line, &capacity, file) == -1 && feof(file));
    free(line);
    fclose(file);
    unlink(template_path);

    unsigned hex = 0;
    char rest[8];
    CHECK(sscanf("0x1f:end", "%x:%7[a-z]", &hex, rest) == 2 && hex == 0x1f && strcmp(rest, "end") == 0);
    CHECK(strtod("2.5e3", nullptr) == 2500.0 && strtod("-0.125", nullptr) == -0.125);
    CHECK(sysconf(_SC_NPROCESSORS_ONLN) == 8 && sysconf(_SC_PAGESIZE) == 4096);
}

void test_users()
{
    struct passwd* root = getpwnam("root");
    CHECK(root && root->pw_uid == 0 && strcmp(root->pw_dir, "/var/root") == 0);
    struct passwd* user = getpwuid(501);
    CHECK(user && strcmp(user->pw_name, "vulture") == 0 && user->pw_gid == 20);
    CHECK(getpwnam("nobody-at-all") == nullptr);
    struct group* staff = getgrgid(20);
    CHECK(staff && strcmp(staff->gr_name, "staff") == 0 && staff->gr_mem[0] && strcmp(staff->gr_mem[1], "vulture") == 0);
    gid_t groups[8];
    int count = 8;
    CHECK(getgrouplist("vulture", 20, groups, &count) >= 0 && count == 2 && groups[0] == 20 && groups[1] == 80);
}

sig_atomic_t volatile s_alarms = 0;

void test_alarm_and_sigsuspend()
{
    struct sigaction action {};
    action.sa_handler = [](int) { s_alarms = s_alarms + 1; };
    sigaction(SIGALRM, &action, nullptr);
    sigset_t block, previous;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    sigprocmask(SIG_BLOCK, &block, &previous);
    CHECK(alarm(1) == 0);
    // While blocked it waits as pending; sigsuspend lets it through.
    sigset_t open_mask = previous;
    sigdelset(&open_mask, SIGALRM);
    CHECK(sigsuspend(&open_mask) == -1 && errno == EINTR);
    CHECK(s_alarms == 1);
    // The mask is back as it was before sigsuspend: SIGALRM blocked.
    sigset_t now;
    sigprocmask(SIG_SETMASK, nullptr, &now);
    CHECK(sigismember(&now, SIGALRM));
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    CHECK(alarm(10) == 0 && alarm(0) >= 9);
}

}

int main()
{
    test_getopt();
    test_patterns();
    test_time();
    test_environment_and_paths();
    test_users();
    test_alarm_and_sigsuspend();
    return finish();
}
