/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// A plugin that wants something nobody has, for Tests/Userland/plugins.cpp.

extern "C" {

int a_function_nobody_has(int);

int plugin_broken(int value)
{
    return a_function_nobody_has(value);
}

}
