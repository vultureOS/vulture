/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

// A plugin, for Tests/Userland/plugins.cpp: it has code, data, thread-local data, an initializer, and needs a library that isn't asked for.

extern "C" {

int plugin_helper_double(int);

int plugin_initialized = 0;
int plugin_value = 41;
thread_local int plugin_thread_value = 1000;
thread_local int plugin_thread_zero = 0;

int plugin_add(int a, int b)
{
    return a + b;
}

// Through what it needs.
int plugin_double_length(char const* text)
{
    return plugin_helper_double(static_cast<int>(strlen(text)));
}

int plugin_call_back(int (*function)(int), int value)
{
    return function(value) + 1;
}

int plugin_next_thread_value()
{
    plugin_thread_zero = plugin_thread_zero + 1;
    return plugin_thread_value++ + plugin_thread_zero;
}

__attribute__((constructor)) static void start_plugin()
{
    plugin_initialized = plugin_helper_double(21);
}

}
