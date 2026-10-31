/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned char s_global[64 * 1024];

static void fill(unsigned char* bytes, size_t size, unsigned char seed)
{
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<unsigned char>(seed + i * 7);
}

static bool holds(unsigned char const* bytes, size_t size, unsigned char seed)
{
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] != static_cast<unsigned char>(seed + i * 7))
            return false;
    }
    return true;
}

int main()
{
    constexpr size_t heap_size = 1024 * 1024;
    auto* heap = static_cast<unsigned char*>(malloc(heap_size));
    CHECK(heap);
    unsigned char stack[8192];
    fill(s_global, sizeof(s_global), 1);
    fill(heap, heap_size, 2);
    fill(stack, sizeof(stack), 3);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        // The child sees the parent's data, then changes its own copy.
        bool same = holds(s_global, sizeof(s_global), 1) && holds(heap, heap_size, 2) && holds(stack, sizeof(stack), 3);
        fill(s_global, sizeof(s_global), 11);
        fill(heap, heap_size, 12);
        fill(stack, sizeof(stack), 13);
        bool changed = holds(s_global, sizeof(s_global), 11) && holds(heap, heap_size, 12) && holds(stack, sizeof(stack), 13);
        _exit(same && changed ? 0 : 1);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    // The child's writes went to its own copies.
    CHECK(holds(s_global, sizeof(s_global), 1));
    CHECK(holds(heap, heap_size, 2));
    CHECK(holds(stack, sizeof(stack), 3));

    // And the parent can still write its own (now unshared) pages.
    fill(heap, heap_size, 4);
    CHECK(holds(heap, heap_size, 4));
    free(heap);
    return finish();
}
