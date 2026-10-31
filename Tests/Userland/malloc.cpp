/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct Node : RefCounted<Node> {
    explicit Node(int v)
        : value(v)
    {
    }
    int value;
};

struct Box {
    explicit Box(int v)
        : value(v)
    {
    }
    int value;
};

}

int main()
{
    // Many sizes, patterns written and checked, freed in a scrambled order.
    constexpr int count = 2000;
    static void* pointers[count];
    static size_t sizes[count];
    unsigned seed = 12345;
    for (int i = 0; i < count; ++i) {
        seed = seed * 1103515245 + 12345;
        sizes[i] = (seed >> 8) % (i % 10 == 0 ? 100'000 : 600) + 1;
        pointers[i] = malloc(sizes[i]);
        CHECK(pointers[i] != nullptr);
        CHECK(reinterpret_cast<uintptr_t>(pointers[i]) % 16 == 0);
        memset(pointers[i], i & 0xff, sizes[i]);
    }
    for (int round = 0; round < 2; ++round) {
        for (int i = round; i < count; i += 2) {
            auto* bytes = static_cast<unsigned char*>(pointers[i]);
            bool intact = bytes[0] == (i & 0xff) && bytes[sizes[i] - 1] == (i & 0xff);
            CHECK(intact);
            free(pointers[i]);
        }
    }

    auto* zeroed = static_cast<unsigned char*>(calloc(1000, 4));
    CHECK(zeroed);
    bool all_zero = true;
    for (int i = 0; i < 4000; ++i)
        all_zero = all_zero && zeroed[i] == 0;
    CHECK(all_zero);

    // realloc keeps the contents.
    auto* grown = static_cast<char*>(malloc(10));
    strcpy(grown, "keep me");
    grown = static_cast<char*>(realloc(grown, 100'000));
    CHECK(grown && strcmp(grown, "keep me") == 0);
    free(grown);
    free(zeroed);

    // A large allocation.
    auto* large = static_cast<unsigned char*>(malloc(16 * 1024 * 1024));
    CHECK(large);
    large[0] = 1;
    large[16 * 1024 * 1024 - 1] = 2;
    free(large);

    // C++: new/delete and LibBase's smart pointers.
    auto* array = new int[100];
    array[99] = 9;
    delete[] array;
    auto owned = MUST(try_make<Box>(5));
    CHECK(owned->value == 5);
    auto shared = MUST(try_make_ref_counted<Node>(6));
    RefPtr<Node> second = shared;
    CHECK(shared->ref_count() == 2 && second->value == 6);
    return finish();
}
