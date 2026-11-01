/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <sys/utsname.h>

// uname [-amnprsv]
int main(int argc, char** argv)
{
    struct utsname name;
    if (uname(&name) < 0) {
        perror("uname");
        return 1;
    }
    bool s = false, n = false, r = false, v = false, m = false, p = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            fprintf(stderr, "usage: uname [-amnprsv]\n");
            return 1;
        }
        for (char const* option = argv[i] + 1; *option; ++option) {
            switch (*option) {
            case 'a':
                s = n = r = v = m = p = true;
                break;
            case 's':
                s = true;
                break;
            case 'n':
                n = true;
                break;
            case 'r':
                r = true;
                break;
            case 'v':
                v = true;
                break;
            case 'm':
                m = true;
                break;
            case 'p':
                p = true;
                break;
            default:
                fprintf(stderr, "usage: uname [-amnprsv]\n");
                return 1;
            }
        }
    }
    if (!(s || n || r || v || m || p))
        s = true;
    bool space = false;
    auto print = [&](bool wanted, char const* text) {
        if (!wanted)
            return;
        printf(space ? " %s" : "%s", text);
        space = true;
    };
    print(s, name.sysname);
    print(n, name.nodename);
    print(r, name.release);
    print(v, name.version);
    print(m, name.machine);
    print(p, "arm");
    putchar('\n');
    return 0;
}
