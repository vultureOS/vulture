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
    commands
    copy-on-write
    crypto
    debugging
    devices
    dynamic
    eventloop
    exec
    exec-target
    file-cache
    files
    many-files
    filesystem
    interactive
    launchd
    fork
    graphics
    hardening
    kernel-fault
    hello
    libc
    libc-more
    locale-files
    malloc
    many-processes
    mapped-files
    memory
    permissions
    pipes
    ports
    compressor
    plugins
    posix
    priority
    privacy
    process-info
    sandbox
    segfault
    services
    sd-card
    shell
    signals
    sockets
    terminals
    threads
    time
    toolkit
    tools
    usb-disk
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
# What Meta/oracle-test.sh compares with what a Mac says.
vulture_add_program(test-locale-print SOURCES ../LibLocale/LocalePrint.cpp DESTINATION usr/tests LIBRARIES LibLocale)
set_target_properties(test-locale-print PROPERTIES OUTPUT_NAME locale-print)
target_link_libraries(test-ports PRIVATE LibIPC LibCore)
target_link_libraries(test-bootstrap PRIVATE LibIPC)
target_link_libraries(test-graphics PRIVATE LibIPC)
target_link_libraries(test-services PRIVATE LibServices)
target_link_libraries(test-windows PRIVATE LibWindow LibImage LibGfx LibIPC LibCore)
target_link_libraries(test-toolkit PRIVATE LibUI LibImage)
target_link_libraries(test-threads PRIVATE LibIPC LibCore)
target_link_libraries(test-crypto PRIVATE LibCrypto)
target_link_libraries(test-privacy PRIVATE LibCrypto)
target_link_libraries(test-dynamic PRIVATE LibCrypto)
target_link_libraries(test-hardening PRIVATE LibCrypto)
vulture_compile_ipc(test-bootstrap Echo.ipc)
# The service bootstrap.cpp starts through vlaunchd.
vulture_add_program(echo-service SOURCES echo-service.cpp DESTINATION usr/tests LIBRARIES LibIPC)
vulture_compile_ipc(echo-service Echo.ipc)

# Libraries that plugins.cpp loads and unloads while it's running, as a program would a plugin.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
function(vulture_add_test_plugin name)
    add_library(${name} SHARED ${name}.cpp)
    set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ".dylib")
    target_link_libraries(${name} PRIVATE ${ARGN} System)
    target_link_options(${name} PRIVATE
        --ld-path=${VULTURE_LLD_ROOT}/bin/ld.lld
        -nostdlib
        -Wl,-Bdynamic
        -Wl,-shared
        -Wl,-soname,@loader_path/${name}.dylib
        -Wl,--hash-style=sysv
        -Wl,-z,now
        -Wl,-z,relro
        -Wl,-z,max-page-size=4096
        -Wl,-z,separate-loadable-segments
        -Wl,--build-id=none)
    set(installed ${CMAKE_CURRENT_BINARY_DIR}/installed/${name}.dylib)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/installed
        COMMAND ${CMAKE_OBJCOPY} --strip-debug $<TARGET_FILE:${name}> ${installed}
        VERBATIM)
    install(PROGRAMS ${installed} DESTINATION usr/tests)
endfunction()
vulture_add_test_plugin(plugin-helper)
vulture_add_test_plugin(plugin-main plugin-helper)
vulture_add_test_plugin(plugin-unload)
vulture_add_test_plugin(plugin-broken)
target_link_options(plugin-broken PRIVATE -Wl,--unresolved-symbols=ignore-all)
