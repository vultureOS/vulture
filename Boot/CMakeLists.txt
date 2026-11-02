#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(VultureBoot
    Arch/${CMAKE_SYSTEM_PROCESSOR}/Handoff.cpp
    Arch/${CMAKE_SYSTEM_PROCESSOR}/PageTableBuilder.cpp
    BootVolume.cpp
    Console.cpp
    KernelLoader.cpp
    Main.cpp
    Memory.cpp
    Support.cpp)

set_target_properties(VultureBoot PROPERTIES
    OUTPUT_NAME ${VULTURE_UEFI_BOOT_NAME}
    SUFFIX ".EFI")

target_link_libraries(VultureBoot PRIVATE LibDeviceTree LibELF LibBase LibBaseFreestanding)

install(TARGETS VultureBoot RUNTIME DESTINATION EFI/BOOT)
