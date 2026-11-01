#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Packs the root filesystem staging tree (ROOT) into the boot archive
# (OUTPUT) with mkarchive (TOOL), in a stable order.

file(GLOB_RECURSE files LIST_DIRECTORIES true RELATIVE ${ROOT} ${ROOT}/*)
# A bracket in a name ("/bin/[") would group list items, so brackets are
# stood in for while the list is worked on; the names go to mkarchive one
# per line.
string(REPLACE "[" "<open-bracket>" files "${files}")
string(REPLACE "]" "<close-bracket>" files "${files}")

# Directories whose mode is not 0755, as macOS has them.
set(directory_modes
    "tmp=1777"
    "var/tmp=1777"
    "Users/Shared=1777"
    "var/root=750")

# Directories go in as "path/<tab>mode", files as "path".
set(entries "")
foreach(path IN LISTS files)
    if(IS_DIRECTORY ${ROOT}/${path})
        set(mode 755)
        foreach(special IN LISTS directory_modes)
            string(REPLACE "=" ";" pair "${special}")
            list(GET pair 0 special_path)
            list(GET pair 1 special_mode)
            if(path STREQUAL special_path)
                set(mode ${special_mode})
            endif()
        endforeach()
        list(APPEND entries "${path}/\t${mode}")
    else()
        list(APPEND entries "${path}")
    endif()
endforeach()
list(SORT entries)
string(REPLACE ";" "\n" listing "${entries}")
string(REPLACE "<open-bracket>" "[" listing "${listing}")
string(REPLACE "<close-bracket>" "]" listing "${listing}")
get_filename_component(output_directory ${OUTPUT} DIRECTORY)
file(MAKE_DIRECTORY ${output_directory})
# The list stays in the build directory (the script runs there), not in the
# ESP beside the archive.
set(list_file ${CMAKE_CURRENT_BINARY_DIR}/boot-archive.files)
file(WRITE ${list_file} "${listing}\n")
execute_process(COMMAND ${TOOL} ${OUTPUT} ${ROOT} @${list_file} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "mkarchive failed")
endif()
