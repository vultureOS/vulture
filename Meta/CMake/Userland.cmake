#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Helpers for the userland part.

# vulture_add_program(<name> SOURCES <files...> [DESTINATION <dir>] [LIBRARIES <libs...>])
#
# A statically linked program (ELF, ET_EXEC at 0x400000), installed into the
# root filesystem at <dir> (without one, only under the names given to
# vulture_install_alias). Each segment gets pages of its own, so the
# kernel can map code read-execute and data read-write without overlap.
# The installed copy has no debug information (the one in the build tree
# keeps it, for debuggers); symbols stay, for crash backtraces.
function(vulture_add_program name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "DESTINATION" "SOURCES;LIBRARIES")
    add_executable(${name} ${arg_SOURCES} $<TARGET_OBJECTS:crt0>)
    target_link_libraries(${name} PRIVATE ${arg_LIBRARIES} LibCxxRT LibC LibBase LibBaseFreestanding)
    target_link_options(${name} PRIVATE
        -Wl,--image-base=0x400000
        -Wl,-z,max-page-size=4096
        -Wl,-z,separate-loadable-segments
        -Wl,--build-id=none)
    # Named as the program is (OUTPUT_NAME may differ from the target's name).
    set(installed ${CMAKE_CURRENT_BINARY_DIR}/installed/$<TARGET_FILE_NAME:${name}>)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/installed
        COMMAND ${CMAKE_OBJCOPY} --strip-debug $<TARGET_FILE:${name}> ${installed}
        VERBATIM)
    set_target_properties(${name} PROPERTIES VULTURE_INSTALLED_FILE ${installed})
    if(arg_DESTINATION)
        install(PROGRAMS ${installed} DESTINATION ${arg_DESTINATION})
    endif()
endfunction()

# vulture_install_alias(<program> <directory> <name>)
#
# Installs <program> a second time, as <directory>/<name>, for programs
# that act by the name they are run as (egrep, whoami, "[").
function(vulture_install_alias program directory alias)
    get_target_property(installed ${program} VULTURE_INSTALLED_FILE)
    install(PROGRAMS ${installed} DESTINATION ${directory} RENAME ${alias})
endfunction()
