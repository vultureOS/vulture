/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Message.h>
#include <NotifyEndpoint.h>
#include <errno.h>
#include <notify.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// notifyutil: posts, watches, gets and sets notifications (notify(3)).
// Commands run in order:
//   -p key       post a notification for key
//   -w key       register for key and report notifications
//   -# key       (# a number, as -1) register for key and report # notifications
//   -g key       get the state value for key
//   -s key val   set the state value for key
//   -port / -file   how later registrations are told (a port, the default; a pipe)

namespace {

bool s_quiet = false;
bool s_verbose = false;
long s_pause_milliseconds = 100;
bool s_use_file = false;

void usage()
{
    fprintf(stderr,
        "usage: notifyutil [-q] [-v] [-z msec] [command ...]\n"
        "    -q             quiet mode\n"
        "    -v             verbose - prints time stamp and notification name\n"
        "    -z msec        pause msec milliseconds after posting [default 100]\n"
        "commands:\n"
        "    -port          switch to mach port for subsequent registrations [default]\n"
        "    -file          switch to file descriptor for subsequent registrations\n"
        "    -p key         post a notification for key\n"
        "    -w key         register for key and report notifications\n"
        "    -# key         (# is an integer value, eg \"-1\") register for key and report # notifications\n"
        "    -g key         get state value for key\n"
        "    -s key val     set state value for key\n");
}

void report(char const* name)
{
    if (s_quiet)
        return;
    if (s_verbose) {
        time_t now = time(nullptr);
        char stamp[32];
        struct tm broken {};
        localtime_r(&now, &broken);
        strftime(stamp, sizeof(stamp), "%b %e %H:%M:%S", &broken);
        printf("%s %s\n", stamp, name);
    } else {
        printf("%s\n", name);
    }
    fflush(stdout);
}

// Waits for `count` posts of `name` (forever when count < 0).
int watch(char const* name, long count)
{
    int token = 0;
    int fd = -1;
    uint32_t status = s_use_file ? notify_register_file_descriptor(name, &fd, 0, &token) : notify_register_port(name, &fd, 0, &token);
    if (status != NOTIFY_STATUS_OK) {
        fprintf(stderr, "notifyutil: registration for %s failed with status %u\n", name, status);
        return 1;
    }
    IPC::Port port(fd);
    for (long seen = 0; count < 0 || seen < count;) {
        pollfd watched { fd, POLLIN, 0 };
        if (poll(&watched, 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        if (s_use_file) {
            unsigned char bytes[4];
            if (read(fd, bytes, sizeof(bytes)) != sizeof(bytes))
                return 1;
        } else {
            auto message = IPC::receive(port);
            if (message.is_error())
                return 1;
        }
        report(name);
        ++seen;
    }
    notify_cancel(token);
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }
    int result = 0;
    for (int i = 1; i < argc; ++i) {
        char const* option = argv[i];
        char const* key = i + 1 < argc ? argv[i + 1] : nullptr;
        if (strcmp(option, "-q") == 0) {
            s_quiet = true;
        } else if (strcmp(option, "-v") == 0) {
            s_verbose = true;
        } else if (strcmp(option, "-z") == 0 && key) {
            s_pause_milliseconds = atol(key);
            ++i;
        } else if (strcmp(option, "-port") == 0) {
            s_use_file = false;
        } else if (strcmp(option, "-file") == 0) {
            s_use_file = true;
        } else if (strcmp(option, "-p") == 0 && key) {
            uint32_t status = notify_post(key);
            if (status != NOTIFY_STATUS_OK) {
                fprintf(stderr, "notifyutil: notify_post %s failed with status %u\n", key, status);
                result = 1;
            }
            if (s_pause_milliseconds > 0) {
                timespec pause { s_pause_milliseconds / 1000, (s_pause_milliseconds % 1000) * 1'000'000 };
                nanosleep(&pause, nullptr);
            }
            ++i;
        } else if (strcmp(option, "-w") == 0 && key) {
            result |= watch(key, -1);
            ++i;
        } else if (option[0] == '-' && option[1] >= '0' && option[1] <= '9' && key) {
            result |= watch(key, atol(option + 1));
            ++i;
        } else if (strcmp(option, "-g") == 0 && key) {
            int token = 0;
            uint64_t state = 0;
            if (notify_register_check(key, &token) != NOTIFY_STATUS_OK || notify_get_state(token, &state) != NOTIFY_STATUS_OK) {
                fprintf(stderr, "notifyutil: could not get the state of %s\n", key);
                result = 1;
            } else {
                printf("%s %llu\n", key, static_cast<unsigned long long>(state));
            }
            notify_cancel(token);
            ++i;
        } else if (strcmp(option, "-s") == 0 && key && i + 2 < argc) {
            int token = 0;
            uint64_t state = strtoull(argv[i + 2], nullptr, 0);
            uint32_t status = notify_register_check(key, &token);
            if (status == NOTIFY_STATUS_OK)
                status = notify_set_state(token, state);
            if (status != NOTIFY_STATUS_OK) {
                fprintf(stderr, "notifyutil: notify_set_state %s failed with status %u\n", key, status);
                result = 1;
            }
            notify_cancel(token);
            i += 2;
        } else {
            usage();
            return 1;
        }
    }
    fflush(stdout);
    return result;
}
