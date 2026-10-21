/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace Base {

// Each environment (kernel, bootloader, libc, host tests) supplies this:
// report the failure and stop. It must never return.
[[noreturn]] void verification_failed(char const* expression, char const* file, unsigned line);

}

// VERIFY stays enabled in every build. A failed check is a bug, and stopping
// is safer than running on with broken invariants.
#define VERIFY(expression) \
    (__builtin_expect(!(expression), 0) ? ::Base::verification_failed(#expression, __FILE_NAME__, __LINE__) : (void)0)

#define VERIFY_NOT_REACHED() ::Base::verification_failed("VERIFY_NOT_REACHED()", __FILE_NAME__, __LINE__)

#define TODO() ::Base::verification_failed("TODO()", __FILE_NAME__, __LINE__)
