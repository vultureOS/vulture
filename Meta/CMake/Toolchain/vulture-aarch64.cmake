#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Target part: freestanding AArch64 ELF code that runs inside vultureOS.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED VULTURE_LLVM_ROOT)
    set(VULTURE_LLVM_ROOT "/opt/homebrew/opt/llvm@22")
endif()
if(NOT DEFINED VULTURE_LLD_ROOT)
    set(VULTURE_LLD_ROOT "/opt/homebrew/opt/lld@22")
endif()

set(CMAKE_CXX_COMPILER ${VULTURE_LLVM_ROOT}/bin/clang++)
set(CMAKE_CXX_COMPILER_TARGET aarch64-unknown-none-elf)
set(CMAKE_ASM_COMPILER ${VULTURE_LLVM_ROOT}/bin/clang)
set(CMAKE_ASM_COMPILER_TARGET aarch64-unknown-none-elf)
set(CMAKE_AR ${VULTURE_LLVM_ROOT}/bin/llvm-ar CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB ${VULTURE_LLVM_ROOT}/bin/llvm-ranlib CACHE FILEPATH "Archive indexer")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path=${VULTURE_LLD_ROOT}/bin/ld.lld -nostdlib -static")
