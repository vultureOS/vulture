#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Programs /sbin/init runs with --test (see Services/Init).
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
    vulture_add_program(test-${test} SOURCES ${test}.cpp DESTINATION usr/tests)
    set_target_properties(test-${test} PROPERTIES OUTPUT_NAME ${test})
endforeach()

# The tests of libraries above LibC.
target_link_libraries(test-eventloop PRIVATE LibCore LibJSON)
