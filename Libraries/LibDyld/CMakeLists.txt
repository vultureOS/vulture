#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The dynamic linker, /usr/lib/dyld: a static program linked for its own
# address (MemoryLayout::user_interpreter_base), with no C library.
add_executable(dyld Start.S dyld.cpp)
target_compile_options(dyld PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-ffreestanding -fno-threadsafe-statics>)
target_link_libraries(dyld PRIVATE LibBaseFreestanding)
target_link_options(dyld PRIVATE
    -Wl,--image-base=0x7f0000000000
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
