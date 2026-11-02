#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Target part: freestanding x86_64 ELF code that runs inside vultureOS.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED VULTURE_LLVM_ROOT)
    set(VULTURE_LLVM_ROOT "/opt/homebrew/opt/llvm@22")
endif()
if(NOT DEFINED VULTURE_LLD_ROOT)
    set(VULTURE_LLD_ROOT "/opt/homebrew/opt/lld@22")
endif()

set(CMAKE_CXX_COMPILER ${VULTURE_LLVM_ROOT}/bin/clang++)
set(CMAKE_CXX_COMPILER_TARGET x86_64-unknown-none-elf)
set(CMAKE_ASM_COMPILER ${VULTURE_LLVM_ROOT}/bin/clang)
set(CMAKE_ASM_COMPILER_TARGET x86_64-unknown-none-elf)
set(CMAKE_AR ${VULTURE_LLVM_ROOT}/bin/llvm-ar CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB ${VULTURE_LLVM_ROOT}/bin/llvm-ranlib CACHE FILEPATH "Archive indexer")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path=${VULTURE_LLD_ROOT}/bin/ld.lld -nostdlib -static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--ld-path=${VULTURE_LLD_ROOT}/bin/ld.lld -nostdlib")

# With no operating system in its target, clang looks for gcc to link with, on this architecture. As far as
# linking goes the target only says which linker driver to use, and nothing here links anything of Linux's.
set(CMAKE_CXX_LINK_EXECUTABLE "<CMAKE_CXX_COMPILER> --target=x86_64-unknown-linux-gnu <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_CREATE_SHARED_LIBRARY "<CMAKE_CXX_COMPILER> --target=x86_64-unknown-linux-gnu <CMAKE_SHARED_LIBRARY_CXX_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> <CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS> <SONAME_FLAG><TARGET_SONAME> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>")
