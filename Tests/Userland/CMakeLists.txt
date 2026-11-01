#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Programs /sbin/vlaunchd runs with --test (see Services/vlaunchd/TestRunner.cpp).
set(tests
    abort
    accounts
    bad-pointers
    copy-on-write
    devices
    eventloop
    exec
    exec-target
    files
    filesystem
    interactive
    launchd
    fork
    hello
    libc
    libc-more
    malloc
    many-processes
    mapped-files
    memory
    permissions
    pipes
    segfault
    shell
    signals
    terminals
    time
    tools
    volumes
    # Not in init's test list: the storage crash and persistence tests run it.
    crash-writer)

foreach(test IN LISTS tests)
    # "hello" stays statically linked, so programs without dyld keep working.
    if(test STREQUAL "hello")
        set(linking STATIC)
    else()
        set(linking "")
    endif()
    vulture_add_program(test-${test} SOURCES ${test}.cpp DESTINATION usr/tests ${linking})
    set_target_properties(test-${test} PROPERTIES OUTPUT_NAME ${test})
endforeach()

# The tests of libraries above LibC.
target_link_libraries(test-eventloop PRIVATE LibCore LibJSON)
