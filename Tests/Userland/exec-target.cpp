/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <stdlib.h>
#include <string.h>

// Run by the exec test: checks what it was given, then exits with 42.
int main(int argc, char** argv)
{
    CHECK(argc == 3);
    CHECK(argc > 0 && strcmp(argv[0], "exec-target") == 0);
    CHECK(argc > 1 && strcmp(argv[1], "one") == 0);
    CHECK(argc > 2 && strcmp(argv[2], "two") == 0);
    CHECK(argv[argc] == nullptr);
    char const* value = getenv("VULTURE_TEST");
    CHECK(value && strcmp(value, "yes") == 0);
    return g_failures ? 1 : 42;
}
