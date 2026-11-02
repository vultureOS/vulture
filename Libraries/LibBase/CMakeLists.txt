#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_library(LibBase STATIC
    Format.cpp)

# Hosted code gets these from the C library.
if(NOT VULTURE_PART STREQUAL "host")
    add_library(LibBaseFreestanding STATIC
        Freestanding/Memory.cpp)
    # These are memcpy and friends: never let the compiler call them here.
    target_compile_options(LibBaseFreestanding PRIVATE -ffreestanding)
    # The sanitizers check on behalf of these (__asan_memcpy and the rest), and call them.
    if(VULTURE_KERNEL_SANITIZERS)
        target_compile_options(LibBaseFreestanding PRIVATE -fno-sanitize=kernel-address,undefined)
    endif()
endif()
