/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// A library that another one needs, for Tests/Userland/plugins.cpp. Nothing asks for it by name: it comes along, and goes when nothing wants it.

extern "C" {

int plugin_helper_initialized = 0;
int* plugin_helper_note_finished = nullptr;

int plugin_helper_double(int value)
{
    return value * 2;
}

__attribute__((constructor)) static void start_helper()
{
    plugin_helper_initialized = 1;
}

__attribute__((destructor)) static void finish_helper()
{
    if (plugin_helper_note_finished)
        *plugin_helper_note_finished = *plugin_helper_note_finished + 1;
}

}
