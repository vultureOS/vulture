#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Packs the root filesystem staging tree (ROOT) into the boot archive
# (OUTPUT) with mkarchive (TOOL), in a stable order.

file(GLOB_RECURSE files LIST_DIRECTORIES true RELATIVE ${ROOT} ${ROOT}/*)
# (Made below, from the rest. Any from an earlier build says nothing about this one. A test of what
# happens to a system that has been tampered with says KEEP_SEAL, for the seal it had before.)
if(NOT KEEP_SEAL)
    list(FILTER files EXCLUDE REGEX "^System/Library/Seal")
endif()
# What a test delivers as a package isn't on the system until it has been installed.
list(FILTER files EXCLUDE REGEX "^usr/tests/package-payload")
# A bracket in a name ("/bin/[") would group list items, so brackets are
# stood in for while the list is worked on; the names go to mkarchive one
# per line.
string(REPLACE "[" "<open-bracket>" files "${files}")
string(REPLACE "]" "<close-bracket>" files "${files}")

# Entries whose mode is not the usual one (0755 for directories and
# programs, 0644 for other files), as macOS has them.
set(special_modes
    "tmp=1777"
    "var/tmp=1777"
    "Users/Shared=1777"
    "var/root=750"
    "var/db/sudo=700"
    "etc/master.passwd=600"
    "usr/bin/login=4555"
    "usr/bin/passwd=4555"
    "usr/bin/su=4755"
    "usr/bin/sudo=4755")

# Directories go in as "path/<tab>mode", files as "path" or
# "path<tab>mode".
set(entries "")
foreach(path IN LISTS files)
    set(mode "")
    foreach(special IN LISTS special_modes)
        string(REPLACE "=" ";" pair "${special}")
        list(GET pair 0 special_path)
        list(GET pair 1 special_mode)
        if(path STREQUAL special_path)
            set(mode ${special_mode})
        endif()
    endforeach()
    if(IS_DIRECTORY ${ROOT}/${path})
        if(mode STREQUAL "")
            set(mode 755)
        endif()
        list(APPEND entries "${path}/\t${mode}")
    elseif(mode STREQUAL "")
        list(APPEND entries "${path}")
    else()
        list(APPEND entries "${path}\t${mode}")
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

# The seal: made from what really went into the archive, and then put in it too.
if(SEAL_TOOL)
    file(MAKE_DIRECTORY ${ROOT}/System/Library/Seal)
    execute_process(COMMAND ${SEAL_TOOL} seal ${OUTPUT} ${SEAL_KEY}
        ${ROOT}/System/Library/Seal/Manifest ${ROOT}/System/Library/Seal/Manifest.sig RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "vseal failed")
    endif()
    file(APPEND ${list_file} "System/Library/Seal/\t755\nSystem/Library/Seal/Manifest\nSystem/Library/Seal/Manifest.sig\n")
    execute_process(COMMAND ${TOOL} ${OUTPUT} ${ROOT} @${list_file} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "mkarchive failed")
    endif()
endif()
