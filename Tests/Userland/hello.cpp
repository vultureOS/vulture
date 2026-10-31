/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    printf("Hello from user space! I am %s, process %d (parent %d), with %d argument%s.\n", argv[0], getpid(), getppid(), argc, argc == 1 ? "" : "s");
    return 0;
}
