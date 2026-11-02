/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <sandbox.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// sandbox-exec -n name | -p text command [arguments...]: runs it in a sandbox, by the name of a profile
// in /System/Library/Sandbox/Profiles, or the rules themselves. See Kernel/Security/Sandbox.h.

int main(int argc, char** argv)
{
    if (argc < 4 || (strcmp(argv[1], "-n") != 0 && strcmp(argv[1], "-p") != 0)) {
        fprintf(stderr, "usage: sandbox-exec -n name | -p text command [arguments...]\n");
        return 64;
    }
    char* error = nullptr;
    if (sandbox_init_for_program(argv[2], strcmp(argv[1], "-n") == 0 ? SANDBOX_NAMED : 0, argv[3], &error) != 0) {
        fprintf(stderr, "sandbox-exec: %s\n", error ? error : "failed");
        return 71;
    }
    execvp(argv[3], argv + 3);
    perror("sandbox-exec");
    return 71;
}
