/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dlfcn.h>
#include <LibSystem/DyldServices.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// All of this is the dynamic linker's to do. What's here is what the linker can't have: a lock, and somewhere to keep each thread's
// last error. A program linked statically has no linker, so nothing loads, and dlerror() says so.

namespace {

System::DyldServices const* s_services = nullptr;

// An initializer can call dlopen(), so this one can be taken again by the thread that has it.
pthread_mutex_t s_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

struct Locked {
    Locked() { pthread_mutex_lock(&s_lock); }
    ~Locked() { pthread_mutex_unlock(&s_lock); }
};

thread_local char* s_error = nullptr;

void set_error(char const* message)
{
    free(s_error);
    s_error = message ? strdup(message) : nullptr;
}

void note_error()
{
    if (auto const* message = s_services->take_error())
        set_error(message);
}

}

extern "C" void __libc_set_dyld_services(System::DyldServices const* services)
{
    s_services = services;
}

extern "C" {

void* dlopen(char const* path, int mode)
{
    if (!s_services) {
        set_error("dlopen: this program is statically linked");
        return nullptr;
    }
    Locked locked;
    void* handle = s_services->open(path, mode);
    if (!handle)
        note_error();
    return handle;
}

int dlclose(void* handle)
{
    if (!s_services) {
        set_error("dlclose: this program is statically linked");
        return -1;
    }
    Locked locked;
    int const result = s_services->close(handle);
    if (result != 0)
        note_error();
    return result;
}

void* dlsym(void* handle, char const* symbol)
{
    if (!s_services) {
        set_error("dlsym: this program is statically linked");
        return nullptr;
    }
    Locked locked;
    void* address = s_services->symbol(handle, symbol, __builtin_return_address(0));
    if (!address)
        note_error();
    return address;
}

char* dlerror(void)
{
    // Once: the next call says nothing, unless there's been another. The last one stays around until then for the caller to read.
    static thread_local char* s_returned = nullptr;
    free(s_returned);
    s_returned = s_error;
    s_error = nullptr;
    return s_returned;
}

int dladdr(void const* address, Dl_info* info)
{
    if (!s_services || !info)
        return 0;
    Locked locked;
    return s_services->address(address, &info->dli_fname, &info->dli_fbase, &info->dli_sname, &info->dli_saddr);
}

}
