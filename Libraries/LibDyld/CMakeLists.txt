#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The dynamic linker, /usr/lib/dyld: a static program, position independent as everything is, that
# relocates itself first thing (Start.S), with no C library. It runs before there is a canary to
# check against, so it goes without; what it reads is checked by being signed.
add_executable(dyld Start.S dyld.cpp)
target_compile_options(dyld PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-ffreestanding -fno-threadsafe-statics -fno-stack-protector>)
target_link_libraries(dyld PRIVATE LibBaseFreestanding)
target_link_options(dyld PRIVATE
    -Wl,-pie
    -Wl,--no-dynamic-linker
    -Wl,-Bsymbolic
    -Wl,-z,notext
    -Wl,--hash-style=sysv
    -Wl,-e,_dyld_start
    -Wl,-z,max-page-size=4096
    -Wl,-z,separate-loadable-segments
    -Wl,--build-id=none)
set(installed ${CMAKE_CURRENT_BINARY_DIR}/installed/dyld)
add_custom_command(TARGET dyld POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/installed
    COMMAND ${CMAKE_OBJCOPY} --strip-debug $<TARGET_FILE:dyld> ${installed}
    VERBATIM)
install(PROGRAMS ${installed} DESTINATION usr/lib)
