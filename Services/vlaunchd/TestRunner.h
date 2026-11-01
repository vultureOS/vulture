/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace Launchd {

// Each runs to the end and powers the machine off.
int run_tests();
int run_storage_test(bool once);

}
