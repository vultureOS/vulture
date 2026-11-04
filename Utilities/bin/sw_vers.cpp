/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

// sw_vers [-productName | -productVersion | -buildVersion]
int main(int argc, char** argv)
{
    utsname name {};
    uname(&name);
    char const* const product = "vultureOS";
    char const* const version = name.release;
    char build[64];
    snprintf(build, sizeof(build), "%s", name.release);
    // A build's name, in the way a Mac's has one.
    for (char* at = build; *at; ++at) {
        if (*at == '.')
            *at = 'A';
    }
    if (argc == 2) {
        if (strcmp(argv[1], "-productName") == 0)
            puts(product);
        else if (strcmp(argv[1], "-productVersion") == 0)
            puts(version);
        else if (strcmp(argv[1], "-buildVersion") == 0)
            puts(build);
        else {
            fprintf(stderr, "usage: sw_vers [-productName|-productVersion|-buildVersion]\n");
            return 1;
        }
        return 0;
    }
    if (argc > 2) {
        fprintf(stderr, "usage: sw_vers [-productName|-productVersion|-buildVersion]\n");
        return 1;
    }
    printf("ProductName:\t\t%s\nProductVersion:\t\t%s\nBuildVersion:\t\t%s\n", product, version, build);
    return 0;
}
