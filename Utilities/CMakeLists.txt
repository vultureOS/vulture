#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The shell, installed as /bin/vsh and as /bin/sh.
vulture_add_program(vsh
    SOURCES vsh/Builtins.cpp vsh/LineEditor.cpp vsh/Parser.cpp vsh/Shell.cpp vsh/main.cpp
    DESTINATION bin)
vulture_install_alias(vsh bin sh)
