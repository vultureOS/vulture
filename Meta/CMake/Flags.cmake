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

add_compile_options(-Wall -Wextra -Werror -g -fdiagnostics-color=always)

# Flags that only mean something to the C++ compiler (assembly files skip them).
function(vulture_add_cxx_options)
    foreach(option IN LISTS ARGN)
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:${option}>)
    endforeach()
endfunction()

vulture_add_cxx_options(
    -Wshadow
    -Wnon-virtual-dtor
    -Wimplicit-fallthrough
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -nostdinc
    -nostdinc++
    -fno-omit-frame-pointer)

add_compile_definitions(VULTURE_VERSION_STRING="${VULTURE_VERSION}")

include_directories(${VULTURE_SOURCE_ROOT} ${VULTURE_SOURCE_ROOT}/Libraries)

if(VULTURE_PART STREQUAL "host")
    # Host tests link only libSystem's C functions (malloc, write, abort);
    # LibTest supplies the C++ runtime pieces.
    vulture_add_cxx_options(-O1 -fsanitize=address,undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined -nostdlib++)
    if(NOT CMAKE_HOST_APPLE)
        # Elsewhere the sanitizers' runtimes are linked in whole, and their
        # C++ parts would bring a second operator new beside LibTest's.
        add_link_options(-fno-sanitize-link-c++-runtime)
    endif()
elseif(VULTURE_PART STREQUAL "uefi")
    vulture_add_cxx_options(-O2 -ffreestanding -fno-stack-protector -mno-stack-arg-probe)
elseif(VULTURE_PART STREQUAL "userland")
    # Programs and their libraries, running at EL0. They may use floating
    # point; frame pointers stay for crash reports. Everything is position
    # independent: programs are PIE, libSystem is shared.
    add_compile_options(-fPIC)
    # Programs choose static or dynamic linking themselves (Userland.cmake).
    string(REPLACE "-static" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    vulture_add_cxx_options(
        -O2
        -fno-stack-protector
        -mno-omit-leaf-frame-pointer
        -mno-outline-atomics
        -fno-unwind-tables
        -fno-asynchronous-unwind-tables)
    include_directories(BEFORE SYSTEM ${VULTURE_SOURCE_ROOT}/Libraries/LibC)
elseif(VULTURE_PART STREQUAL "target")
    # The kernel keeps frame pointers in every function, leaves included, so
    # backtraces never skip a frame; it uses no floating point.
    vulture_add_cxx_options(
        -O2
        -ffreestanding
        -fno-stack-protector
        -mgeneral-regs-only
        -mno-omit-leaf-frame-pointer
        -mno-outline-atomics
        -fno-unwind-tables
        -fno-asynchronous-unwind-tables)
endif()
