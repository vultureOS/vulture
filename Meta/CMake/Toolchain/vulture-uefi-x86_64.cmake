#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# UEFI part: VultureBoot as a PE32+ EFI application for x86_64. UEFI uses the
# PE/COFF format, so this part compiles for the Windows (GNU) target and links
# with lld-link directly. The GNU flavor keeps the Itanium C++ ABI the rest of
# vultureOS uses, and CMake treats it as an ordinary clang (the MSVC flavor
# would make CMake drop --target and dependency tracking).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED VULTURE_LLVM_ROOT)
    set(VULTURE_LLVM_ROOT "/opt/homebrew/opt/llvm@22")
endif()
if(NOT DEFINED VULTURE_LLD_ROOT)
    set(VULTURE_LLD_ROOT "/opt/homebrew/opt/lld@22")
endif()

set(CMAKE_CXX_COMPILER ${VULTURE_LLVM_ROOT}/bin/clang++)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-windows-gnu)
set(CMAKE_AR ${VULTURE_LLVM_ROOT}/bin/llvm-ar CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB ${VULTURE_LLVM_ROOT}/bin/llvm-ranlib CACHE FILEPATH "Archive indexer")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_CXX_LINK_EXECUTABLE "${VULTURE_LLD_ROOT}/bin/lld-link <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES> /out:<TARGET>")
# Firmware relocates the image anyway; a zero base stops it from first trying
# (and logging a failure) to allocate the default 0x140000000.
set(CMAKE_EXE_LINKER_FLAGS_INIT "/nologo /nodefaultlib /subsystem:efi_application /entry:efi_main /base:0")

# Firmware looks for \EFI\BOOT\BOOT<arch>.EFI on removable media.
set(VULTURE_UEFI_BOOT_NAME BOOTX64)
