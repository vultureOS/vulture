#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Helpers for the userland part.

# vulture_add_program(<name> SOURCES <files...> DESTINATION <dir> [LIBRARIES <libs...>])
#
# A statically linked program (ELF, ET_EXEC at 0x400000), installed into the
# root filesystem at <dir>. Each segment gets pages of its own, so the
# kernel can map code read-execute and data read-write without overlap.
function(vulture_add_program name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "DESTINATION" "SOURCES;LIBRARIES")
    add_executable(${name} ${arg_SOURCES} $<TARGET_OBJECTS:crt0>)
    target_link_libraries(${name} PRIVATE ${arg_LIBRARIES} LibCxxRT LibC LibBase LibBaseFreestanding)
    target_link_options(${name} PRIVATE
        -Wl,--image-base=0x400000
        -Wl,-z,max-page-size=4096
        -Wl,-z,separate-loadable-segments
        -Wl,--build-id=none)
    install(TARGETS ${name} RUNTIME DESTINATION ${arg_DESTINATION})
endfunction()
