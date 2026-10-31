#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Packs the root filesystem staging tree (ROOT) into the boot archive
# (OUTPUT) with mkarchive (TOOL), in a stable order.

file(GLOB_RECURSE files RELATIVE ${ROOT} ${ROOT}/*)
list(SORT files)
get_filename_component(output_directory ${OUTPUT} DIRECTORY)
file(MAKE_DIRECTORY ${output_directory})
execute_process(COMMAND ${TOOL} ${OUTPUT} ${ROOT} ${files} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "mkarchive failed")
endif()
