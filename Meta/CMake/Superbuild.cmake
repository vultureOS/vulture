#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Drives each build part as an ExternalProject (same source tree, separate
# binary directory and toolchain), then assembles the boot volume.

include(ExternalProject)

# LLVM 22 where Homebrew (Apple silicon or Intel) or apt.llvm.org puts it.
set(vulture_default_llvm_root "/opt/homebrew/opt/llvm@22")
set(vulture_default_lld_root "/opt/homebrew/opt/lld@22")
foreach(candidate /opt/homebrew/opt /usr/local/opt)
    if(EXISTS ${candidate}/llvm@22/bin/clang++)
        set(vulture_default_llvm_root ${candidate}/llvm@22)
        set(vulture_default_lld_root ${candidate}/lld@22)
        break()
    endif()
endforeach()
if(NOT EXISTS ${vulture_default_llvm_root}/bin/clang++ AND EXISTS /usr/lib/llvm-22/bin/clang++)
    set(vulture_default_llvm_root /usr/lib/llvm-22)
    set(vulture_default_lld_root /usr/lib/llvm-22)
endif()
set(VULTURE_LLVM_ROOT ${vulture_default_llvm_root} CACHE PATH "LLVM 22 installation providing clang")
set(VULTURE_LLD_ROOT ${vulture_default_lld_root} CACHE PATH "LLD 22 installation providing ld.lld and lld-link")

set(VULTURE_ESP_DIR ${CMAKE_BINARY_DIR}/esp)

# vulture_add_part(<part> TOOLCHAIN <name> [INSTALL_DIR <dir>] [DEPENDS <parts...>])
function(vulture_add_part part)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "TOOLCHAIN;INSTALL_DIR" "DEPENDS")
    set(depends_args "")
    if(arg_DEPENDS)
        set(depends_args DEPENDS ${arg_DEPENDS})
    endif()

    if(arg_INSTALL_DIR)
        set(install_args INSTALL_DIR ${arg_INSTALL_DIR})
        set(install_prefix -DCMAKE_INSTALL_PREFIX=${arg_INSTALL_DIR})
    else()
        # An empty INSTALL_COMMAND would vanish when the list is expanded.
        set(install_args INSTALL_COMMAND ${CMAKE_COMMAND} -E true)
        set(install_prefix "")
    endif()

    ExternalProject_Add(${part}
        SOURCE_DIR ${CMAKE_SOURCE_DIR}
        PREFIX ${CMAKE_BINARY_DIR}/.superbuild/${part}
        BINARY_DIR ${CMAKE_BINARY_DIR}/${part}
        CMAKE_ARGS
            --no-warn-unused-cli
            -DVULTURE_PART=${part}
            -DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/Meta/CMake/Toolchain/${arg_TOOLCHAIN}.cmake
            -DVULTURE_LLVM_ROOT=${VULTURE_LLVM_ROOT}
            -DVULTURE_LLD_ROOT=${VULTURE_LLD_ROOT}
            -DVULTURE_HOST_TOOLS=${CMAKE_BINARY_DIR}/host/Tools
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
            ${install_prefix}
        BUILD_ALWAYS ON
        USES_TERMINAL_CONFIGURE ON
        USES_TERMINAL_BUILD ON
        USES_TERMINAL_INSTALL ON
        ${depends_args}
        ${install_args})
endfunction()

vulture_add_part(host TOOLCHAIN host)
# Both install into the EFI System Partition staging tree: the loader in
# EFI/BOOT, the kernel in System/Library/Kernels.
vulture_add_part(target TOOLCHAIN vulture-aarch64 INSTALL_DIR ${VULTURE_ESP_DIR})
vulture_add_part(uefi TOOLCHAIN vulture-uefi-aarch64 INSTALL_DIR ${VULTURE_ESP_DIR})
# Programs install into the root filesystem staging tree, which is packed
# into the boot archive the kernel mounts as /.
set(VULTURE_ROOT_DIR ${CMAKE_BINARY_DIR}/root)
# It needs the host's ipcc for its IPC endpoints.
vulture_add_part(userland TOOLCHAIN vulture-aarch64 INSTALL_DIR ${VULTURE_ROOT_DIR} DEPENDS host)

# The root also has the kernel where macOS keeps it (the loader boots the
# copy on the ESP), without its debug information.
add_custom_target(boot-archive ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VULTURE_ROOT_DIR}/System/Library/Kernels
    COMMAND ${VULTURE_LLVM_ROOT}/bin/llvm-objcopy --strip-debug
        ${VULTURE_ESP_DIR}/System/Library/Kernels/vulture ${VULTURE_ROOT_DIR}/System/Library/Kernels/vulture
    COMMAND ${CMAKE_COMMAND}
        -DTOOL=${CMAKE_BINARY_DIR}/host/Tools/mkarchive/mkarchive
        -DROOT=${VULTURE_ROOT_DIR}
        -DOUTPUT=${VULTURE_ESP_DIR}/System/Library/Kernels/vulture.bootarchive
        -P ${CMAKE_SOURCE_DIR}/Meta/CMake/MakeBootArchive.cmake
    DEPENDS host target userland
    VERBATIM)

# One compile database for the whole tree, for clangd and other editors.
add_custom_target(compile-commands ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${CMAKE_BINARY_DIR}/compile_commands.json
        "-DINPUTS=${CMAKE_BINARY_DIR}/host/compile_commands.json$<SEMICOLON>${CMAKE_BINARY_DIR}/uefi/compile_commands.json$<SEMICOLON>${CMAKE_BINARY_DIR}/target/compile_commands.json$<SEMICOLON>${CMAKE_BINARY_DIR}/userland/compile_commands.json"
        -P ${CMAKE_SOURCE_DIR}/Meta/CMake/MergeCompileCommands.cmake
    DEPENDS host uefi target userland
    VERBATIM)

add_custom_target(test-host
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}/host --output-on-failure
    DEPENDS host
    USES_TERMINAL
    VERBATIM)

add_custom_target(test-boot
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless
        --expect "Memory map"
        --expect "8 of 8 processors online"
        --expect "vultureKernel: boot complete"
        --expect "vlaunchd: vultureOS userland is up (process 1)"
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

# A deliberate fault must produce a symbolized, demangled backtrace.
add_custom_target(test-panic
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless
        --boot-args "fault=data-abort"
        --expect "KERNEL PANIC: Data abort (write) at 0x0000000000000010: translation fault, level 0"
        --expect "Kernel::trigger_deliberate_fault"
        --expect "System halted."
    DEPENDS host uefi target boot-archive test-boot
    USES_TERMINAL
    VERBATIM)

# The in-kernel tests ("ktest"): allocators, locks, timers and processors
# under stress on all 8 virtual CPUs. The guest powers itself off afterwards.
add_custom_target(test-kernel
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless
        --boot-args "ktest"
        --wait-for-exit
        --timeout 120
        --expect "ktest: PASS"
    DEPENDS host uefi target boot-archive test-panic
    USES_TERMINAL
    VERBATIM)

# Userland: /sbin/vlaunchd --test forks and execs the test programs in
# /usr/tests (fork, copy-on-write, exec, signals, memory, files...), checks
# how each ended, and powers the machine off.
add_custom_target(test-userland
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless
        --boot-args "utest"
        --wait-for-exit
        --timeout 120
        --expect "userland: PASS"
    DEPENDS host uefi target boot-archive test-kernel
    USES_TERMINAL
    VERBATIM)

# A text login on the console, typed over the serial line: vlaunchd starts
# getty, login lets the user in, the shell runs a pipeline and a job
# suspended and continued; logging out brings getty back; sudo poweroff
# shuts down through vlaunchd.
add_custom_target(test-login
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless
        --chat ${CMAKE_SOURCE_DIR}/Tests/Console/login.chat
        --wait-for-exit
        --timeout 120
    DEPENDS host uefi target boot-archive test-userland
    USES_TERMINAL
    VERBATIM)

# Storage: files written by one boot are there for the next; vultureFS
# checked with fsck.vfs after each.
add_custom_target(test-persistence
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/storage-test.sh --build-dir ${CMAKE_BINARY_DIR} persistence
    DEPENDS host uefi target boot-archive test-userland
    USES_TERMINAL
    VERBATIM)

add_custom_target(check DEPENDS test-host test-boot test-panic test-kernel test-userland test-login test-persistence)

# M5's "done when": 100 power cuts at random moments while writing; each
# must leave a disk that passes fsck and remounts with every committed
# file intact. Not part of check, for its length.
add_custom_target(test-crash
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/storage-test.sh --build-dir ${CMAKE_BINARY_DIR} crash 100
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

# The scheduler soak (M3's "done when"): 1000 threads ping-ponging across
# every processor for 60 seconds, then all stopped and joined. Not part of
# check, for its length.
add_custom_target(test-soak
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless
        --boot-args "ktest=scheduler.thousand ktest-seconds=60"
        --wait-for-exit
        --timeout 180
        --expect "ktest: PASS"
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

# Performance numbers: context switches, round trips, thread creation.
add_custom_target(bench
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless
        --boot-args "ktest=bench"
        --wait-for-exit
        --timeout 60
        --expect "ktest: PASS"
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

# One bootable disk image with an EFI System Partition and vultureFS, as a
# real machine would have (Meta/run.sh --image boots it).
add_custom_target(image
    COMMAND ${CMAKE_COMMAND}
        -DTOOL=${CMAKE_BINARY_DIR}/host/Tools/vfs/mkimage
        -DESP=${VULTURE_ESP_DIR}
        -DARCHIVE=${VULTURE_ESP_DIR}/System/Library/Kernels/vulture.bootarchive
        -DOUTPUT=${CMAKE_BINARY_DIR}/vultureOS.img
        -P ${CMAKE_SOURCE_DIR}/Meta/CMake/MakeDiskImage.cmake
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

add_custom_target(run
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)
