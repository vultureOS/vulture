/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>

// A plugin that goes away properly, for Tests/Userland/plugins.cpp: it has things to do when it does, whichever way they were asked for.

namespace {

int* s_note = nullptr;

struct StaticObject {
    ~StaticObject()
    {
        if (s_note)
            *s_note = *s_note + 1;
    }
    int value { 7 };
};

StaticObject s_object;

void at_exit_handler()
{
    if (s_note)
        *s_note = *s_note + 10;
}

__attribute__((destructor)) void finish()
{
    if (s_note)
        *s_note = *s_note + 100;
}

}

extern "C" {

int plugin_unload_value()
{
    return s_object.value;
}

void plugin_unload_set_note(int* note)
{
    s_note = note;
    atexit(at_exit_handler);
}

}
