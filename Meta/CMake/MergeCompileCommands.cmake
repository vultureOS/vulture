#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Merges the compile_commands.json of every build part into one database so
# editors (clangd) understand the whole tree.
#
#   cmake -DOUTPUT=<file> -DINPUTS=<file;file;...> -P MergeCompileCommands.cmake

set(merged "")
foreach(input IN LISTS INPUTS)
    if(NOT EXISTS "${input}")
        continue()
    endif()
    file(READ "${input}" entries)
    string(STRIP "${entries}" entries)
    string(REGEX REPLACE "^\\[" "" entries "${entries}")
    string(REGEX REPLACE "\\]$" "" entries "${entries}")
    string(STRIP "${entries}" entries)
    if(entries STREQUAL "")
        continue()
    endif()
    if(NOT merged STREQUAL "")
        string(APPEND merged ",\n")
    endif()
    string(APPEND merged "${entries}")
endforeach()

file(WRITE "${OUTPUT}.tmp" "[\n${merged}\n]\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
