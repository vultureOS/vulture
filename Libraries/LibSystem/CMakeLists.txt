#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# libSystem.B.dylib, as macOS calls its C library: our C library, the C++
# runtime and LibBase's compiled parts in one shared library every program
# links with. dyld loads it; every process shares its pages.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
add_library(System SHARED DsoHandle.S)
set_target_properties(System PROPERTIES PREFIX "lib" OUTPUT_NAME "System.B" SUFFIX ".dylib")
target_link_libraries(System PRIVATE
    -Wl,--whole-archive LibC LibCxxRT LibBase LibBaseFreestanding -Wl,--no-whole-archive)
target_link_options(System PRIVATE
    --ld-path=${VULTURE_LLD_ROOT}/bin/ld.lld
    -nostdlib
    -Wl,-Bdynamic
    -Wl,-shared
    -Wl,-soname,libSystem.B.dylib
    -Wl,--hash-style=sysv
    -Wl,-z,now
    -Wl,-z,relro
    -Wl,-z,max-page-size=4096
    -Wl,-z,separate-loadable-segments
    -Wl,--build-id=none)
set(installed ${CMAKE_CURRENT_BINARY_DIR}/installed/libSystem.B.dylib)
add_custom_command(TARGET System POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/installed
    COMMAND ${CMAKE_OBJCOPY} --strip-debug $<TARGET_FILE:System> ${installed}
    VERBATIM)
install(FILES ${installed} DESTINATION usr/lib PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
