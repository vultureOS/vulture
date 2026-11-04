/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_LOCAL 0x4
#define RTLD_GLOBAL 0x8
#define RTLD_NOLOAD 0x10
#define RTLD_NODELETE 0x80

// What's special to dlsym: everything after the one calling, everything, that and everything after it, or only the program.
#define RTLD_NEXT ((void*)-1)
#define RTLD_DEFAULT ((void*)-2)
#define RTLD_SELF ((void*)-3)
#define RTLD_MAIN_ONLY ((void*)-5)

typedef struct dl_info {
    char const* dli_fname;
    void* dli_fbase;
    char const* dli_sname;
    void* dli_saddr;
} Dl_info;

void* dlopen(char const* path, int mode);
int dlclose(void* handle);
void* dlsym(void* handle, char const* symbol);
char* dlerror(void);
int dladdr(void const* address, Dl_info*);

__END_DECLS
