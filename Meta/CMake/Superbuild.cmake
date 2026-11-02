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

# What to build for: a build directory holds one. (cmake -B build-x86_64 -DVULTURE_ARCH=x86_64)
set(VULTURE_ARCH aarch64 CACHE STRING "The architecture to build for: aarch64 or x86_64")
set_property(CACHE VULTURE_ARCH PROPERTY STRINGS aarch64 x86_64)

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
# (The kernel needs a host tool for the key the system is sealed with.)
vulture_add_part(target TOOLCHAIN vulture-${VULTURE_ARCH} INSTALL_DIR ${VULTURE_ESP_DIR} DEPENDS host)
vulture_add_part(uefi TOOLCHAIN vulture-uefi-${VULTURE_ARCH} INSTALL_DIR ${VULTURE_ESP_DIR})
# Programs install into the root filesystem staging tree, which is packed
# into the boot archive the kernel mounts as /.
set(VULTURE_ROOT_DIR ${CMAKE_BINARY_DIR}/root)
# It needs the host's ipcc for its IPC endpoints.
vulture_add_part(userland TOOLCHAIN vulture-${VULTURE_ARCH} INSTALL_DIR ${VULTURE_ROOT_DIR} DEPENDS host)

# The root also has the kernel where macOS keeps it (the loader boots the
# copy on the ESP), without its debug information.
add_custom_target(boot-archive ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VULTURE_ROOT_DIR}/System/Library/Kernels
    COMMAND ${VULTURE_LLVM_ROOT}/bin/llvm-objcopy --strip-debug
        ${VULTURE_ESP_DIR}/System/Library/Kernels/vulture ${VULTURE_ROOT_DIR}/System/Library/Kernels/vulture
    COMMAND ${CMAKE_COMMAND}
        -DTOOL=${CMAKE_BINARY_DIR}/host/Tools/mkarchive/mkarchive
        -DROOT=${VULTURE_ROOT_DIR}
        -DSEAL_TOOL=${CMAKE_BINARY_DIR}/host/Tools/vseal/vseal
        -DSEAL_KEY=${CMAKE_BINARY_DIR}/signing/system.key
        -DOUTPUT=${VULTURE_ESP_DIR}/System/Library/Kernels/vulture.bootarchive
        -P ${CMAKE_SOURCE_DIR}/Meta/CMake/MakeBootArchive.cmake
    DEPENDS host target userland
    VERBATIM)

# What a Raspberry Pi's firmware starts (and, for QEMU, which hasn't that firmware, a description of the machine).
if(VULTURE_ARCH STREQUAL "aarch64")
    set(VULTURE_RASPBERRY_PI_DIR ${CMAKE_BINARY_DIR}/raspberry-pi)
    add_custom_target(raspberry-pi ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${VULTURE_RASPBERRY_PI_DIR}
        COMMAND ${CMAKE_BINARY_DIR}/host/Tools/mkpiimage/mkpiimage
            ${CMAKE_BINARY_DIR}/target/Boot/RaspberryPi/raspberry-pi-stub.bin
            ${VULTURE_ESP_DIR}/System/Library/Kernels/vulture
            ${VULTURE_ESP_DIR}/System/Library/Kernels/vulture.bootarchive
            ${VULTURE_RASPBERRY_PI_DIR}/kernel8.img
        COMMAND ${CMAKE_BINARY_DIR}/host/Tools/mkfdt/mkfdt
            ${CMAKE_SOURCE_DIR}/Tests/Hardware/raspi4b.dts
            ${VULTURE_RASPBERRY_PI_DIR}/raspi4b.dtb
        DEPENDS boot-archive
        VERBATIM)
endif()

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

add_custom_target(test-keyboard
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless
        --chat ${CMAKE_SOURCE_DIR}/Tests/Console/keyboard.chat
        --wait-for-exit
        --timeout 120
    DEPENDS host uefi target boot-archive test-login
    USES_TERMINAL
    VERBATIM)

# Golden screenshots: the scene in Tests/UI, composed by the window server, must
# match what is in Tests/UI/Golden (Meta/ui-test.sh --update, after looking).
add_custom_target(test-ui
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/ui-test.sh --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS host uefi target boot-archive test-keyboard
    USES_TERMINAL
    VERBATIM)

# What Meta/run.sh and Meta/run_debug.sh boot. (--gui: headless, it would be the console.)
add_custom_target(test-desktop-boot
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless --gui
        --expect "loginwindow: ready"
        --timeout 60
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)
add_custom_target(test-debug-boot
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless --gui --debug
        --expect "process: 1 execs /sbin/vlaunchd"
        --expect "vlaunchd: (com.vulture.WindowServer"
        --expect "[com.vulture.windowserver:server] Starting"
        --expect "loginwindow: ready"
        --timeout 60
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

# What the sound device played, as QEMU recorded it.
add_custom_target(test-sound
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/sound-test.sh --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

# What the kernel's protections do when tested on purpose.
add_custom_target(test-hardening
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/hardening-test.sh --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

# The drivers for what real machines have, on QEMU's models of it.
add_custom_target(test-hardware
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/hardware-test.sh --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

# Using more memory than the machine has, and running out with nothing left to compress.
add_custom_target(test-compressor
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/compressor-test.sh --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

# The kernel, built again with UBSan and KASAN-style heap checks: they must catch what is planted,
# and find nothing in the kernel's tests or the userland's.
add_custom_target(test-sanitized-kernel
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/sanitized-kernel-test.sh --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

# TCP, DNS, TLS, HTTP and signed packages, against servers on this machine. And then the real
# thing, for those with an internet connection: not in check, as not everyone has one to hand.
add_custom_target(test-network
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/network-test.sh --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)
add_custom_target(test-internet
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless
        --chat ${CMAKE_SOURCE_DIR}/Tests/Network/internet.chat
        --timeout 90
    DEPENDS host uefi target boot-archive
    USES_TERMINAL
    VERBATIM)

add_custom_target(test-raspberry-pi
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/raspberry-pi-test.sh --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS host uefi target boot-archive raspberry-pi
    USES_TERMINAL
    VERBATIM)

add_custom_target(check DEPENDS test-host test-boot test-panic test-kernel test-userland test-login test-keyboard test-desktop-boot test-debug-boot test-sound test-network test-hardening test-compressor test-hardware test-raspberry-pi test-sanitized-kernel test-ui test-persistence)

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
