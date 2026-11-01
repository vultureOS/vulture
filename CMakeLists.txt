#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# vultureOS is built in "parts", each with its own toolchain:
#
#   host    tools and unit tests that run on the build machine
#   uefi    VultureBoot, the UEFI loader (PE32+)
#   target    the kernel (ELF, AArch64)
#   userland  programs and libraries that run in user mode (ELF, AArch64)
#
# Configuring this directory without VULTURE_PART sets up the superbuild,
# which drives every part and provides the run/test targets.

cmake_minimum_required(VERSION 3.25)

set(VULTURE_VERSION 0.1.0)

if(NOT DEFINED VULTURE_PART)
    project(vultureOS VERSION ${VULTURE_VERSION} LANGUAGES NONE)
    include(Meta/CMake/Superbuild.cmake)
    return()
endif()

project(vultureOS-${VULTURE_PART} VERSION ${VULTURE_VERSION} LANGUAGES CXX)

set(VULTURE_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include(Meta/CMake/Flags.cmake)

if(VULTURE_PART STREQUAL "host")
    enable_testing()
    add_subdirectory(Libraries/LibArchive)
    add_subdirectory(Libraries/LibBase)
    add_subdirectory(Libraries/LibCrypto)
    add_subdirectory(Libraries/LibDeviceTree)
    add_subdirectory(Libraries/LibELF)
    add_subdirectory(Libraries/LibJSON)
    add_subdirectory(Libraries/LibTest)
    add_subdirectory(Libraries/LibVultureFS)
    add_subdirectory(Tests)
    add_subdirectory(Tools)
elseif(VULTURE_PART STREQUAL "uefi")
    add_subdirectory(Libraries/LibBase)
    add_subdirectory(Libraries/LibDeviceTree)
    add_subdirectory(Libraries/LibELF)
    add_subdirectory(Boot)
elseif(VULTURE_PART STREQUAL "target")
    enable_language(ASM)
    add_subdirectory(Libraries/LibBase)
    add_subdirectory(Libraries/LibDeviceTree)
    add_subdirectory(Libraries/LibArchive)
    add_subdirectory(Libraries/LibELF)
    add_subdirectory(Libraries/LibVultureFS)
    add_subdirectory(Kernel)
elseif(VULTURE_PART STREQUAL "userland")
    enable_language(ASM)
    include(Meta/CMake/Userland.cmake)
    add_subdirectory(Libraries/LibBase)
    add_subdirectory(Libraries/LibC)
    add_subdirectory(Libraries/LibCore)
    add_subdirectory(Libraries/LibCrypto)
    add_subdirectory(Libraries/LibCxxRT)
    add_subdirectory(Libraries/LibDyld)
    add_subdirectory(Libraries/LibJSON)
    add_subdirectory(Libraries/LibSystem)
    add_subdirectory(Services)
    add_subdirectory(Utilities)
    add_subdirectory(Tests/Userland)
    # The root filesystem skeleton: /etc and friends, and the macOS
    # hierarchy's directories even while nothing is in them (their modes
    # are in Meta/CMake/MakeBootArchive.cmake).
    install(DIRECTORY Base/ DESTINATION .)
    foreach(directory
            Applications Library Library/Preferences Library/LaunchDaemons Library/LaunchAgents
            System/Library/CoreServices System/Library/Frameworks System/Library/LaunchDaemons System/Library/LaunchAgents
            Users Users/Shared Volumes dev tmp
            usr/local usr/local/bin usr/share
            var var/root var/log var/run var/tmp var/db var/db/sudo)
        install(DIRECTORY DESTINATION ${directory})
    endforeach()
else()
    message(FATAL_ERROR "Unknown VULTURE_PART '${VULTURE_PART}'")
endif()
