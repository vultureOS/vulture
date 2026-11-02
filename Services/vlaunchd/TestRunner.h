/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace Launchd {

// Each runs to the end and powers the machine off.
// Every test, or only the one named (as in /usr/tests/NAME).
int run_tests(char const* only = nullptr);
int run_storage_test(bool once);

}
