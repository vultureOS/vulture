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
    getopt.cpp
    glob.cpp
    identity.cpp
    libgen.cpp
    locale.cpp
    malloc.cpp
    pwd.cpp
    regex.cpp
    resource.cpp
    setjmp.S
    signal.cpp
    spawn.cpp
    start.cpp
    stdio.cpp
    scanf.cpp
    stdlib.cpp
    stdlib_extra.cpp
    string.cpp
    string_extra.cpp
    syscalls.cpp
    termios.cpp
    time.cpp
    unistd_extra.cpp)

# The C library implements the functions the compiler would otherwise turn
# its loops into calls to.
target_compile_options(LibC PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-ffreestanding>)
target_link_libraries(LibC PUBLIC LibBaseFreestanding)
