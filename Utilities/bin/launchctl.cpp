/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <BootstrapEndpoint.h>
#include <LibBase/StringView.h>
#include <LibIPC/Bootstrap.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// launchctl: the jobs vlaunchd looks after.
//
//   launchctl list [LABEL]
//   launchctl start LABEL
//   launchctl stop LABEL
//   launchctl load PATH
//   launchctl unload PATH

static int usage()
{
    fprintf(stderr, "usage: launchctl list [label] | start label | stop label | load path | unload path\n");
    return 64;
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return usage();
    auto port = IPC::bootstrap_port();
    if (port.is_error()) {
        fprintf(stderr, "launchctl: no launch daemon to talk to\n");
        return 1;
    }
    BootstrapProxy launchd(port.release_value());
    char const* verb = argv[1];

    if (strcmp(verb, "list") == 0) {
        auto answer = launchd.list_jobs();
        if (answer.is_error()) {
            fprintf(stderr, "launchctl: could not ask\n");
            return 1;
        }
        auto const listing = answer.value().view();
        if (argc == 2) {
            fwrite(listing.characters_without_null_termination(), 1, listing.length(), stdout);
            return 0;
        }
        // One of them: as it says about one, which is a list of what's known.
        size_t start = 0;
        while (start < listing.length()) {
            size_t end = start;
            while (end < listing.length() && listing[end] != '\n')
                ++end;
            auto line = listing.substring_view(start, end - start);
            auto last_tab = line.find_last('\t');
            if (last_tab.has_value() && line.substring_view(last_tab.value() + 1) == StringView(argv[2], strlen(argv[2]))) {
                auto first_tab = line.find('\t');
                auto pid = line.substring_view(0, first_tab.value());
                auto status = line.substring_view(first_tab.value() + 1, last_tab.value() - first_tab.value() - 1);
                printf("{\n");
                printf("\t\"LimitLoadToSessionType\" = \"System\";\n");
                printf("\t\"Label\" = \"%s\";\n", argv[2]);
                printf("\t\"OnDemand\" = true;\n");
                printf("\t\"LastExitStatus\" = %.*s;\n", static_cast<int>(status.length()), status.characters_without_null_termination());
                if (pid != "-"_sv)
                    printf("\t\"PID\" = %.*s;\n", static_cast<int>(pid.length()), pid.characters_without_null_termination());
                printf("};\n");
                return 0;
            }
            start = end + 1;
        }
        fprintf(stderr, "Could not find service \"%s\" in domain for system\n", argv[2]);
        return 113;
    }

    if (argc != 3)
        return usage();
    char const* what = argv[2];
    char absolute[PATH_MAX];
    if (strcmp(verb, "load") == 0 || strcmp(verb, "unload") == 0) {
        if (!realpath(what, absolute)) {
            fprintf(stderr, "launchctl: couldn't stat(\"%s\"): %s\n", what, strerror(errno));
            return 1;
        }
        what = absolute;
    } else if (strcmp(verb, "start") != 0 && strcmp(verb, "stop") != 0) {
        return usage();
    }
    auto answer = launchd.control_job({ verb, strlen(verb) }, { what, strlen(what) });
    if (answer.is_error()) {
        fprintf(stderr, "launchctl: could not ask\n");
        return 1;
    }
    switch (answer.value()) {
    case 0:
        return 0;
    case EEXIST:
        fprintf(stderr, "%s: service already loaded\n", what);
        return 1;
    case ESRCH:
        fprintf(stderr, "Could not find service \"%s\" in domain for system\n", what);
        return 113;
    case EPERM:
        fprintf(stderr, "Not privileged to %s service\n", verb);
        return 1;
    default:
        fprintf(stderr, "launchctl: %s %s: %s\n", verb, what, strerror(answer.value()));
        return 1;
    }
}
