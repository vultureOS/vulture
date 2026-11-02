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
    # Floating point gives the same bits on the host and in the OS (a tool and
    # the system draw the same pixels), and square roots are one instruction.
    -ffp-contract=off
    -fno-math-errno
    -fno-exceptions
    -fno-rtti
    -nostdinc
    -nostdinc++
    -fno-omit-frame-pointer)

add_compile_definitions(VULTURE_VERSION_STRING="${VULTURE_VERSION}")

include_directories(${VULTURE_SOURCE_ROOT} ${VULTURE_SOURCE_ROOT}/Libraries)

if(VULTURE_PART STREQUAL "host")
    # Host tests link only libSystem's C functions (malloc, write, abort);
    # LibTest supplies the C++ runtime pieces.
    vulture_add_cxx_options(-O1 -fno-threadsafe-statics -fsanitize=address,undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined -nostdlib++)
    if(NOT CMAKE_HOST_APPLE)
        # Elsewhere the sanitizers' runtimes are linked in whole, and their
        # C++ parts would bring a second operator new beside LibTest's.
        add_link_options(-fno-sanitize-link-c++-runtime)
    endif()
elseif(VULTURE_PART STREQUAL "uefi")
    vulture_add_cxx_options(-O2 -ffreestanding -fno-threadsafe-statics -fno-stack-protector -mno-stack-arg-probe)
elseif(VULTURE_PART STREQUAL "userland")
    # Programs and their libraries, running at EL0. They may use floating
    # point; frame pointers stay for crash reports. Everything is position
    # independent: programs are PIE, libSystem is shared.
    add_compile_options(-fPIC)
    # Programs choose static or dynamic linking themselves (Userland.cmake).
    string(REPLACE "-static" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    vulture_add_cxx_options(
        -O2
        -fstack-protector-strong
        -mstack-protector-guard=global
        -mno-omit-leaf-frame-pointer
        -fno-unwind-tables
        -fno-asynchronous-unwind-tables)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        vulture_add_cxx_options(
            -mbranch-protection=pac-ret+bti
            -mno-outline-atomics)
    else()
        # All thread-local storage is allocated when a program starts, so there's no need for
        # a function to look it up.
        vulture_add_cxx_options(-ftls-model=initial-exec)
    endif()
    include_directories(BEFORE SYSTEM ${VULTURE_SOURCE_ROOT}/Libraries/LibC)
elseif(VULTURE_PART STREQUAL "target")
    # The kernel keeps frame pointers in every function, leaves included, so
    # backtraces never skip a frame; it uses no floating point.
    vulture_add_cxx_options(
        -O2
        -ffreestanding
        -fno-threadsafe-statics
        -fPIE
        -fstack-protector-strong
        -mstack-protector-guard=global
        -mgeneral-regs-only
        -mno-omit-leaf-frame-pointer
        -fno-unwind-tables
        -fno-asynchronous-unwind-tables)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        vulture_add_cxx_options(
            -mbranch-protection=pac-ret+b-key+bti
            -mno-outline-atomics)
    else()
        # Interrupts run on the same stack, so nothing can be kept below the stack pointer.
        vulture_add_cxx_options(-mno-red-zone)
    endif()
    # A second flavor of the kernel, for tests (Meta/sanitized-kernel-test.sh): undefined behavior
    # and heap mistakes end in a panic. The checks go through functions, Kernel/Debug/Sanitizers.cpp,
    # rather than shadow memory at a fixed address.
    if(VULTURE_KERNEL_SANITIZERS)
        add_compile_definitions(VULTURE_KERNEL_SANITIZERS=1)
        vulture_add_cxx_options(
            -fsanitize=undefined,kernel-address
            -fno-sanitize=vptr,function
            -fsanitize-recover=undefined
            "SHELL:-mllvm -asan-instrumentation-with-call-threshold=0"
            "SHELL:-mllvm -asan-stack=0"
            "SHELL:-mllvm -asan-globals=0"
            "SHELL:-mllvm -asan-instrument-dynamic-allocas=0"
            "SHELL:-mllvm -asan-kernel-mem-intrinsic-prefix=1")
    endif()
endif()
