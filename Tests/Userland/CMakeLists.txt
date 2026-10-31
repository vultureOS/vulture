#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Programs /sbin/init runs with --test (see Services/Init).
set(tests
    abort
    bad-pointers
    copy-on-write
    exec
    exec-target
    files
    fork
    hello
    libc
    malloc
    many-processes
    memory
    segfault
    signals
    time)

foreach(test IN LISTS tests)
    vulture_add_program(test-${test} SOURCES ${test}.cpp DESTINATION usr/tests)
    set_target_properties(test-${test} PROPERTIES OUTPUT_NAME ${test})
endforeach()
