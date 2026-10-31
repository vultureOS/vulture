#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

set(linker_script ${CMAKE_CURRENT_SOURCE_DIR}/Arch/aarch64/linker.ld)

add_executable(Kernel
    Arch/aarch64/Boot.S
    Arch/aarch64/CPU.cpp
    Arch/aarch64/Exceptions.cpp
    Arch/aarch64/Vectors.S
    BootArguments.cpp
    Console/BootScreen.cpp
    Console/Font.cpp
    Console/Framebuffer.cpp
    Console/TextConsole.cpp
    Debug/KernelLog.cpp
    Debug/Panic.cpp
    Debug/Symbols.cpp
    Devices/Serial/PL011.cpp
    Init.cpp
    Library/CxxRuntime.cpp)

set_target_properties(Kernel PROPERTIES
    OUTPUT_NAME vulture
    SUFFIX ""
    LINK_DEPENDS ${linker_script})

target_link_options(Kernel PRIVATE
    -T ${linker_script}
    -Wl,-z,max-page-size=4096
    -Wl,--build-id=none)

target_link_libraries(Kernel PRIVATE LibDeviceTree LibELF LibBase LibBaseFreestanding)

install(TARGETS Kernel RUNTIME DESTINATION System/Library/Kernels)
