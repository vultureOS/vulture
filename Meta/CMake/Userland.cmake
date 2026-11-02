#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Helpers for the userland part.

# vulture_add_program(<name> SOURCES <files...> [DESTINATION <dir>] [LIBRARIES <libs...>] [STATIC])
#
# A program, installed into the root filesystem at <dir> (without one,
# only under the names given to vulture_install_alias). It is position
# independent (loaded at 0x400000 for now), linked with
# /usr/lib/libSystem.B.dylib, which /usr/lib/dyld loads; STATIC links the
# C library into it instead (an ET_EXEC at 0x400000). Each segment gets
# pages of its own, so code is read-execute and data read-write without
# overlap. The installed copy has no debug information (the one in the
# build tree keeps it, for debuggers); symbols stay, for crash backtraces.
function(vulture_add_program name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "STATIC" "DESTINATION" "SOURCES;LIBRARIES")
    add_executable(${name} ${arg_SOURCES} $<TARGET_OBJECTS:crt0>)
    target_link_options(${name} PRIVATE
        -Wl,-z,max-page-size=4096
        -Wl,-z,separate-loadable-segments
        -Wl,--build-id=none)
    if(arg_STATIC)
        target_link_libraries(${name} PRIVATE ${arg_LIBRARIES} LibCxxRT LibC LibBase LibBaseFreestanding)
        target_link_options(${name} PRIVATE -static -Wl,--image-base=0x400000)
    else()
        target_link_libraries(${name} PRIVATE ${arg_LIBRARIES} System)
        target_link_options(${name} PRIVATE
            -Wl,-Bdynamic
            -Wl,-pie
            -Wl,--dynamic-linker=/usr/lib/dyld
            -Wl,--hash-style=sysv
            -Wl,-z,now
            -Wl,-z,relro)
    endif()
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

# vulture_compile_ipc(<target> <file.ipc>...)
#
# Runs ipcc (built by the host part) on each endpoint description, making
# <file>Endpoint.h in the target's build directory, which its sources (and
# those of what links it) can include as <file>Endpoint.h.
function(vulture_compile_ipc target)
    foreach(source IN LISTS ARGN)
        get_filename_component(stem ${source} NAME_WE)
        get_filename_component(input ${source} ABSOLUTE)
        set(output ${CMAKE_CURRENT_BINARY_DIR}/${stem}Endpoint.h)
        add_custom_command(
            OUTPUT ${output}
            COMMAND ${VULTURE_HOST_TOOLS}/ipcc/ipcc ${input} ${output}
            DEPENDS ${input} ${VULTURE_HOST_TOOLS}/ipcc/ipcc
            COMMENT "ipcc ${source}"
            VERBATIM)
        target_sources(${target} PRIVATE ${output})
    endforeach()
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
endfunction()

# vulture_add_application(<target> SOURCES <files...> NAME <name> IDENTIFIER <id> ICON <icon>
#                         [KIND <what it is>] [DESTINATION <dir>] [EXTENSIONS <extensions...>]
#                         [SANDBOX <a profile in /System/Library/Sandbox/Profiles>]
#                         [LIBRARIES <libs...>])
#
# An application bundle: <dir>/<name>.app, with Contents/MacOS/<target> and an Info.plist
# saying what LibDesktop needs to know. Under /Applications, unless told otherwise.
function(vulture_add_application target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAME;IDENTIFIER;ICON;KIND;DESTINATION;SANDBOX" "SOURCES;LIBRARIES;EXTENSIONS")
    if(NOT arg_DESTINATION)
        set(arg_DESTINATION Applications)
    endif()
    vulture_add_program(${target} SOURCES ${arg_SOURCES} DESTINATION "${arg_DESTINATION}/${arg_NAME}.app/Contents/MacOS" LIBRARIES ${arg_LIBRARIES})
    set(extensions "")
    foreach(extension IN LISTS arg_EXTENSIONS)
        if(extensions)
            string(APPEND extensions ", ")
        endif()
        string(APPEND extensions "\"${extension}\"")
    endforeach()
    set(plist "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist")
    file(WRITE "${plist}" "{\n    \"CFBundleName\" : \"${arg_NAME}\",\n    \"CFBundleIdentifier\" : \"${arg_IDENTIFIER}\",\n    \"CFBundleExecutable\" : \"${target}\",\n    \"CFBundleIconFile\" : \"${arg_ICON}\",\n    \"VultureKind\" : \"${arg_KIND}\",\n    \"VultureSandbox\" : \"${arg_SANDBOX}\",\n    \"CFBundleDocumentExtensions\" : [ ${extensions} ]\n}\n")
    install(FILES "${plist}" DESTINATION "${arg_DESTINATION}/${arg_NAME}.app/Contents" RENAME Info.plist)
endfunction()
