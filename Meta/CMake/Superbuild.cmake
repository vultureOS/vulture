#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Drives each build part as an ExternalProject (same source tree, separate
# binary directory and toolchain), then assembles the boot volume.

include(ExternalProject)

set(VULTURE_LLVM_ROOT "/opt/homebrew/opt/llvm@22" CACHE PATH "LLVM 22 installation providing clang")
set(VULTURE_LLD_ROOT "/opt/homebrew/opt/lld@22" CACHE PATH "LLD 22 installation providing ld.lld and lld-link")

set(VULTURE_ESP_DIR ${CMAKE_BINARY_DIR}/esp)

# vulture_add_part(<part> TOOLCHAIN <name> [INSTALL_DIR <dir>])
function(vulture_add_part part)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "TOOLCHAIN;INSTALL_DIR" "")

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
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
            ${install_prefix}
        BUILD_ALWAYS ON
        USES_TERMINAL_CONFIGURE ON
        USES_TERMINAL_BUILD ON
        USES_TERMINAL_INSTALL ON
        ${install_args})
endfunction()

vulture_add_part(host TOOLCHAIN host)
# Both install into the EFI System Partition staging tree: the loader in
# EFI/BOOT, the kernel in System/Library/Kernels.
vulture_add_part(target TOOLCHAIN vulture-aarch64 INSTALL_DIR ${VULTURE_ESP_DIR})
vulture_add_part(uefi TOOLCHAIN vulture-uefi-aarch64 INSTALL_DIR ${VULTURE_ESP_DIR})

# One compile database for the whole tree, for clangd and other editors.
add_custom_target(compile-commands ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT=${CMAKE_BINARY_DIR}/compile_commands.json
        "-DINPUTS=${CMAKE_BINARY_DIR}/host/compile_commands.json$<SEMICOLON>${CMAKE_BINARY_DIR}/uefi/compile_commands.json$<SEMICOLON>${CMAKE_BINARY_DIR}/target/compile_commands.json"
        -P ${CMAKE_SOURCE_DIR}/Meta/CMake/MergeCompileCommands.cmake
    DEPENDS host uefi target
    VERBATIM)

add_custom_target(test-host
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}/host --output-on-failure
    DEPENDS host
    USES_TERMINAL
    VERBATIM)

add_custom_target(test-boot
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless
        --expect "Memory map"
        --expect "vultureKernel: boot complete"
    DEPENDS uefi target
    USES_TERMINAL
    VERBATIM)

# A deliberate fault must produce a symbolized, demangled backtrace.
add_custom_target(test-panic
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR} --headless
        --boot-args "fault=data-abort"
        --expect "KERNEL PANIC: Data abort (write) at 0x0000000000000010: translation fault, level 0"
        --expect "Kernel::trigger_deliberate_fault"
        --expect "System halted."
    DEPENDS uefi target test-boot
    USES_TERMINAL
    VERBATIM)

add_custom_target(check DEPENDS test-host test-boot test-panic)

add_custom_target(run
    COMMAND ${CMAKE_SOURCE_DIR}/Meta/run.sh --build-dir ${CMAKE_BINARY_DIR}
    DEPENDS uefi target
    USES_TERMINAL
    VERBATIM)
