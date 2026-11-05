#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

add_subdirectory(Kernel)
add_subdirectory(LibAnimation)
add_subdirectory(LibGesture)
add_subdirectory(LibArchive)
add_subdirectory(LibAudio)
add_subdirectory(LibBase)
add_subdirectory(LibACPI)
add_subdirectory(LibCompress)
add_subdirectory(LibCrypto)
add_subdirectory(LibDeviceTree)
add_subdirectory(LibELF)
add_subdirectory(LibGfx)
add_subdirectory(LibHTTP)
add_subdirectory(LibImage)
add_subdirectory(LibJSON)
add_subdirectory(LibLocale)
add_subdirectory(LibPackage)
add_subdirectory(LibText)
add_subdirectory(LibTLS)
add_subdirectory(LibUI)
add_subdirectory(LibUnicode)
add_subdirectory(LibVT)
add_subdirectory(Applications)
add_subdirectory(LibForeignFS)
add_subdirectory(LibVultureFS)
add_subdirectory(Tools)
add_subdirectory(WindowServer)

# The Parity Ledger's rules: Meta/parity.py.
find_program(VULTURE_PYTHON python3)
if(VULTURE_PYTHON)
    add_test(NAME Parity COMMAND ${VULTURE_PYTHON} -I ${VULTURE_SOURCE_ROOT}/Tests/Parity/test_parity.py)
endif()
