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

# Accounts: logging in, changing users and passwords.
vulture_add_program(login SOURCES bin/login.cpp DESTINATION usr/bin LIBRARIES LibCore)
vulture_add_program(su SOURCES bin/su.cpp DESTINATION usr/bin LIBRARIES LibCore)
vulture_add_program(passwd SOURCES bin/passwd.cpp DESTINATION usr/bin LIBRARIES LibCore)
vulture_add_program(sudo SOURCES bin/sudo.cpp DESTINATION usr/bin LIBRARIES LibCore)
vulture_add_program(getty SOURCES bin/getty.cpp DESTINATION usr/libexec)

# The system services' tools.
vulture_add_program(log SOURCES bin/log.cpp DESTINATION usr/bin LIBRARIES LibServices)
vulture_add_program(logger SOURCES bin/logger.cpp DESTINATION usr/bin LIBRARIES LibServices)
vulture_add_program(notifyutil SOURCES bin/notifyutil.cpp DESTINATION usr/bin LIBRARIES LibServices)
vulture_add_program(scutil SOURCES bin/scutil.cpp DESTINATION usr/sbin LIBRARIES LibServices)
vulture_add_program(open SOURCES bin/open.cpp DESTINATION usr/bin LIBRARIES LibDesktop)
vulture_add_program(pbcopy SOURCES bin/pbcopy.cpp DESTINATION usr/bin LIBRARIES LibServices)
vulture_install_alias(pbcopy usr/bin pbpaste)
vulture_add_program(afplay SOURCES bin/afplay.cpp DESTINATION usr/bin LIBRARIES LibAudio LibServices)
vulture_add_program(curl SOURCES bin/curl.cpp DESTINATION usr/bin LIBRARIES LibHTTP)
vulture_add_program(pkg SOURCES bin/pkg.cpp DESTINATION usr/bin LIBRARIES LibPackage)
vulture_add_program(shasum SOURCES bin/shasum.cpp DESTINATION usr/bin LIBRARIES LibCrypto)

# vulture_add_tool(<name> <directory> [<other names> <directory>]...):
# bin/<name>.cpp, installed in <directory> (and under the other names,
# for tools that act by the name they are run as).
function(vulture_add_tool name directory)
    vulture_add_program(${name} SOURCES bin/${name}.cpp DESTINATION ${directory})
    set(aliases ${ARGN})
    while(aliases)
        list(POP_FRONT aliases alias alias_directory)
        vulture_install_alias(${name} ${alias_directory} ${alias})
    endwhile()
endfunction()

# Where macOS keeps them: the essentials in /bin, the rest in /usr/bin.
vulture_add_tool(cat bin)
vulture_add_tool(chmod bin)
vulture_add_tool(cp bin)
vulture_add_tool(date bin)
vulture_add_tool(df bin)
vulture_add_tool(hostname bin)
vulture_add_tool(host usr/bin)
vulture_add_tool(ifconfig sbin)
vulture_add_tool(nc usr/bin)
vulture_add_tool(netstat usr/sbin)
vulture_add_tool(ping sbin)
vulture_add_tool(kill bin)
vulture_add_tool(ln bin)
vulture_add_tool(ls bin)
vulture_add_tool(mkdir bin)
vulture_add_tool(mv bin)
vulture_add_tool(ps bin)
vulture_add_tool(rm bin)
vulture_add_tool(rmdir bin)
vulture_add_tool(sleep bin)
vulture_add_tool(test bin)
# "[" cannot go through a CMake list (brackets group list items).
vulture_install_alias(test bin "[")
vulture_add_tool(chown usr/sbin chgrp usr/bin)
vulture_add_tool(cut usr/bin)
vulture_add_tool(du usr/bin)
vulture_add_tool(env usr/bin)
vulture_add_tool(find usr/bin)
vulture_add_tool(grep usr/bin egrep usr/bin fgrep usr/bin)
vulture_add_tool(head usr/bin)
vulture_add_tool(hexdump usr/bin xxd usr/bin)
vulture_add_tool(id usr/bin whoami usr/bin groups usr/bin)
vulture_add_tool(more usr/bin less usr/bin)
vulture_add_tool(printf usr/bin)
vulture_add_tool(sed usr/bin)
vulture_add_tool(seq usr/bin)
vulture_add_tool(sort usr/bin)
vulture_add_tool(stat usr/bin)
vulture_add_tool(tail usr/bin)
vulture_add_tool(tee usr/bin)
vulture_add_tool(time usr/bin)
vulture_add_tool(touch usr/bin)
vulture_add_tool(tr usr/bin)
vulture_add_tool(uname usr/bin)
vulture_add_tool(uniq usr/bin)
vulture_add_tool(uptime usr/bin)
vulture_add_tool(wc usr/bin)
vulture_add_tool(which usr/bin)
vulture_add_tool(xargs usr/bin)
vulture_add_tool(yes usr/bin)
# One program for the small tools, installed only under their names.
vulture_add_program(misc SOURCES bin/misc.cpp)
set(misc_names
    echo bin pwd bin sync bin stty bin
    true usr/bin false usr/bin basename usr/bin dirname usr/bin readlink usr/bin realpath usr/bin mktemp usr/bin clear usr/bin
    tty usr/bin nproc usr/bin logname usr/bin
    sysctl usr/sbin
    reboot sbin halt sbin poweroff sbin shutdown sbin)
while(misc_names)
    list(POP_FRONT misc_names alias alias_directory)
    vulture_install_alias(misc ${alias_directory} ${alias})
endwhile()
