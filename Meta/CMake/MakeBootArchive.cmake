#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Makes the system image (OUTPUT) from the root filesystem staging tree (ROOT): its files go into an
# archive with mkarchive (TOOL), in a stable order; that goes onto a volume with mkvfs (MKVFS); and vseal
# (SEAL_TOOL) makes that a signed system image, with SEAL_KEY.

file(GLOB_RECURSE files LIST_DIRECTORIES true RELATIVE ${ROOT} ${ROOT}/*)
# A bracket in a name ("/bin/[") would group list items, so brackets are
# stood in for while the list is worked on; the names go to mkarchive one
# per line.
string(REPLACE "[" "<open-bracket>" files "${files}")
string(REPLACE "]" "<close-bracket>" files "${files}")
# (Made below, from the rest. Any from an earlier build says nothing about this one.)
list(FILTER files EXCLUDE REGEX "^System/Library/Seal")
# What a test delivers as a package isn't on the system until it has been installed.
list(FILTER files EXCLUDE REGEX "^usr/tests/package-payload")

# Entries whose mode is not the usual one (0755 for directories and
# programs, 0644 for other files), as macOS has them.
set(special_modes
    "cores=1777"
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
# These stay in the build directory (the script runs there), not in the ESP beside the image.
set(list_file ${CMAKE_CURRENT_BINARY_DIR}/boot-archive.files)
set(archive ${CMAKE_CURRENT_BINARY_DIR}/system.tar)
set(volume ${CMAKE_CURRENT_BINARY_DIR}/system.volume)
file(WRITE ${list_file} "${listing}\n")
execute_process(COMMAND ${TOOL} ${archive} ${ROOT} @${list_file} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "mkarchive failed")
endif()

# What may run, wherever a copy of it is: made from what really went in, and then put in too.
file(MAKE_DIRECTORY ${ROOT}/System/Library/Seal)
execute_process(COMMAND ${SEAL_TOOL} list ${archive} ${ROOT}/System/Library/Seal/Manifest RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "vseal failed")
endif()
file(APPEND ${list_file} "System/Library/Seal/\t755\nSystem/Library/Seal/Manifest\n")
execute_process(COMMAND ${TOOL} ${archive} ${ROOT} @${list_file} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "mkarchive failed")
endif()

execute_process(COMMAND ${MKVFS} system ${volume} ${archive} RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "mkvfs failed")
endif()
execute_process(COMMAND ${SEAL_TOOL} seal ${volume} ${SEAL_KEY} ${OUTPUT} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "vseal failed")
endif()
file(REMOVE ${volume})
