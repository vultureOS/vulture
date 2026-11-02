#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# Signed software packages: what a repository offers, and putting it on the system. What only
# looks at bytes also builds for the host, for its tests.
set(LibPackage_SOURCES Catalog.cpp)
if(VULTURE_PART STREQUAL "userland")
    list(APPEND LibPackage_SOURCES Manager.cpp)
endif()
add_library(LibPackage STATIC ${LibPackage_SOURCES})
target_link_libraries(LibPackage PUBLIC LibJSON LibCrypto LibBase)
target_include_directories(LibPackage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(VULTURE_PART STREQUAL "userland")
    vulture_compile_ipc(LibPackage Installer.ipc)
    target_link_libraries(LibPackage PUBLIC LibHTTP LibArchive LibCompress LibServices LibIPC)
endif()
