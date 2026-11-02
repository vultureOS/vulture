#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The Raspberry Pi's firmware knows nothing of UEFI. It loads kernel8.img and jumps to it, so that is this: a stub,
# which does what VultureBoot does, followed by the kernel and the boot archive. (mkpiimage puts them together.)

set(linker_script ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld)

add_executable(RaspberryPiStub
    ${VULTURE_SOURCE_ROOT}/Boot/Arch/aarch64/Handoff.cpp
    ${VULTURE_SOURCE_ROOT}/Boot/Arch/aarch64/PageTableBuilder.cpp
    ${VULTURE_SOURCE_ROOT}/Boot/KernelLoader.cpp
    Entry.S
    Main.cpp)

set_target_properties(RaspberryPiStub PROPERTIES
    OUTPUT_NAME raspberry-pi-stub
    SUFFIX ".elf"
    LINK_DEPENDS ${linker_script})

target_compile_options(RaspberryPiStub PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-stack-protector>)
target_link_options(RaspberryPiStub PRIVATE -T ${linker_script} -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=4096)
target_link_libraries(RaspberryPiStub PRIVATE LibDeviceTree LibELF LibBase LibBaseFreestanding)

add_custom_command(TARGET RaspberryPiStub POST_BUILD
    COMMAND ${VULTURE_LLVM_ROOT}/bin/llvm-objcopy -O binary $<TARGET_FILE:RaspberryPiStub> ${CMAKE_CURRENT_BINARY_DIR}/raspberry-pi-stub.bin
    VERBATIM)
