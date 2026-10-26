#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_executable(VultureBoot
    Console.cpp
    Main.cpp
    Support.cpp)

set_target_properties(VultureBoot PROPERTIES
    OUTPUT_NAME ${VULTURE_UEFI_BOOT_NAME}
    SUFFIX ".EFI")

target_link_libraries(VultureBoot PRIVATE LibBase LibBaseFreestanding)

install(TARGETS VultureBoot RUNTIME DESTINATION EFI/BOOT)
