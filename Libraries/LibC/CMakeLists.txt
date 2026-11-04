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
    dlfcn.cpp
    Encodings.cpp
    fenv.cpp
    FloatText.cpp
    Generated/EncodingTables.cpp
    getopt.cpp
    glob.cpp
    identity.cpp
    libgen.cpp
    langinfo.cpp
    locale.cpp
    malloc.cpp
    math.cpp
    netdb.cpp
    posix_more.cpp
    pthread.cpp
    pwd.cpp
    regex.cpp
    resource.cpp
    sandbox.cpp
    setjmp.S
    signal.cpp
    socket.cpp
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
    unistd_extra.cpp
    wchar.cpp
    wctype.cpp
    # What wctype.cpp asks, and nothing else of Unicode's. (Programs that want the rest have LibUnicode, which comes with its own copy.)
    ../LibUnicode/Generated/CaseMapping.cpp
    ../LibUnicode/Generated/EastAsianWidth.cpp
    ../LibUnicode/Generated/GeneralCategory.cpp)

# The C library implements the functions the compiler would otherwise turn
# its loops into calls to.
target_compile_options(LibC PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-ffreestanding>)
target_link_libraries(LibC PUBLIC LibBaseFreestanding)
