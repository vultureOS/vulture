#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# crt0 is linked into every program first; see vulture_add_program().
add_library(crt0 OBJECT crt0.S)

add_library(LibC STATIC
    ctype.cpp
    dirent.cpp
    malloc.cpp
    setjmp.S
    signal.cpp
    start.cpp
    stdio.cpp
    stdlib.cpp
    string.cpp
    syscalls.cpp
    termios.cpp)

# The C library implements the functions the compiler would otherwise turn
# its loops into calls to.
target_compile_options(LibC PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-ffreestanding>)
target_link_libraries(LibC PUBLIC LibBaseFreestanding)
