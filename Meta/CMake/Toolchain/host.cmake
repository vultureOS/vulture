#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host part: tools and unit tests that run on the build machine.

if(NOT DEFINED VULTURE_LLVM_ROOT)
    set(VULTURE_LLVM_ROOT "/opt/homebrew/opt/llvm@22")
endif()

set(CMAKE_CXX_COMPILER ${VULTURE_LLVM_ROOT}/bin/clang++)

# Homebrew ships per-target clang config files that can name an SDK which is
# not installed; ignore them and pass the SDK explicitly below.
set(CMAKE_CXX_FLAGS_INIT "--no-default-config")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--no-default-config")

# Homebrew clang needs the macOS SDK to find libSystem when linking.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_SYSROOT)
    execute_process(
        COMMAND xcrun --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE vulture_macos_sdk
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(vulture_macos_sdk)
        set(CMAKE_OSX_SYSROOT ${vulture_macos_sdk})
    endif()
endif()
