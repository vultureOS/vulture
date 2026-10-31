/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

// Crashes on purpose: the kernel must kill only this process (with
// SIGSEGV) and print a crash report.

[[gnu::noinline]] static void write_through(int volatile* pointer)
{
    *pointer = 42;
}

[[gnu::noinline]] static void crash()
{
    write_through(nullptr);
}

int main()
{
    printf("segfault: writing through a null pointer\n");
    fflush(stdout);
    crash();
    printf("segfault: still alive?!\n");
    return 1;
}
