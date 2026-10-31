#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Makes a whole bootable disk image (OUTPUT) with mkimage (TOOL): the files
# of the EFI System Partition staging tree (ESP) and the root filesystem
# from the boot archive (ARCHIVE).

file(GLOB_RECURSE files RELATIVE ${ESP} ${ESP}/*)
# run.sh's boot arguments are for its own runs only.
list(FILTER files EXCLUDE REGEX "boot-args$")
list(SORT files)
execute_process(COMMAND ${TOOL} ${OUTPUT} 4096 ${ESP} ${ARCHIVE} ${files} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "mkimage failed")
endif()
