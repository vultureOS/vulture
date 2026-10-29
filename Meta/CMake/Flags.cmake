#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compiler flags shared by every part, then per-part additions. No part sees
# system headers or the C++ standard library: everything comes from our tree.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wnon-virtual-dtor
    -Wimplicit-fallthrough
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -nostdinc
    -nostdinc++
    -fno-omit-frame-pointer
    -g
    -fdiagnostics-color=always)

add_compile_definitions(VULTURE_VERSION_STRING="${VULTURE_VERSION}")

include_directories(${VULTURE_SOURCE_ROOT} ${VULTURE_SOURCE_ROOT}/Libraries)

if(VULTURE_PART STREQUAL "host")
    # Host tests link only libSystem's C functions (malloc, write, abort);
    # LibTest supplies the C++ runtime pieces.
    add_compile_options(-O1 -fsanitize=address,undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined -nostdlib++)
elseif(VULTURE_PART STREQUAL "uefi")
    add_compile_options(-O2 -ffreestanding -fno-stack-protector -mno-stack-arg-probe)
elseif(VULTURE_PART STREQUAL "target")
    add_compile_options(-O2 -ffreestanding -fno-stack-protector -mgeneral-regs-only)
endif()
