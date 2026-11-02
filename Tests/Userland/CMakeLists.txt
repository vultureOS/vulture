#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Programs /sbin/vlaunchd runs with --test (see Services/vlaunchd/TestRunner.cpp).
set(tests
    abort
    accounts
    bootstrap
    bad-pointers
    copy-on-write
    crypto
    devices
    dynamic
    eventloop
    exec
    exec-target
    files
    filesystem
    interactive
    launchd
    fork
    graphics
    hello
    libc
    libc-more
    malloc
    many-processes
    mapped-files
    memory
    permissions
    pipes
    ports
    posix
    process-info
    segfault
    services
    shell
    signals
    sockets
    terminals
    threads
    time
    toolkit
    tools
    volumes
    windows
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
target_link_libraries(test-ports PRIVATE LibIPC LibCore)
target_link_libraries(test-bootstrap PRIVATE LibIPC)
target_link_libraries(test-graphics PRIVATE LibIPC)
target_link_libraries(test-services PRIVATE LibServices)
target_link_libraries(test-windows PRIVATE LibWindow LibImage LibGfx LibIPC LibCore)
target_link_libraries(test-toolkit PRIVATE LibUI LibImage)
target_link_libraries(test-threads PRIVATE LibIPC LibCore)
target_link_libraries(test-crypto PRIVATE LibCrypto)
vulture_compile_ipc(test-bootstrap Echo.ipc)
# The service bootstrap.cpp starts through vlaunchd.
vulture_add_program(echo-service SOURCES echo-service.cpp DESTINATION usr/tests LIBRARIES LibIPC)
vulture_compile_ipc(echo-service Echo.ipc)
