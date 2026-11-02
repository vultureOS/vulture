/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// What the processor offers for cryptography, as the kernel reports it. (A Mac's own
// programs can't ask this way, so tests on one run the plain code; Tests/Userland/crypto.cpp
// runs both on the real system, and compares them.)

#if defined(__aarch64__) && !defined(__APPLE__)
#    define VULTURE_CRYPTO_HAS_ARM_INSTRUCTIONS 1

extern "C" unsigned long getauxval(unsigned long type);

namespace Crypto {

inline constexpr unsigned long auxiliary_hardware_capabilities = 16; // AT_HWCAP
inline constexpr unsigned long capability_aes = 1 << 3;
inline constexpr unsigned long capability_pmull = 1 << 4;

}

#endif
