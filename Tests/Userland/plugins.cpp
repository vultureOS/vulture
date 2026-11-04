/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// dlopen(): code that a program didn't start with, and can do without again.

namespace {

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            char const* said_so = dlerror();                                                   \
            fprintf(stderr, "plugins: line %d: %s: %s\n", __LINE__, #condition, said_so ? said_so : strerror(errno)); \
            exit(1);                                                                           \
        }                                                                                      \
    } while (0)

char const* const s_plugin = "/usr/tests/plugin-main.dylib";
char const* const s_unload = "/usr/tests/plugin-unload.dylib";
char const* const s_helper = "/usr/tests/plugin-helper.dylib";

template<typename T>
T look_up(void* handle, char const* name)
{
    auto* found = dlsym(handle, name);
    CHECK(found != nullptr);
    return reinterpret_cast<T>(found);
}

int times_three(int value)
{
    return value * 3;
}

void loading()
{
    CHECK(dlopen(s_plugin, RTLD_NOW | RTLD_NOLOAD) == nullptr);
    (void)dlerror();

    void* plugin = dlopen(s_plugin, RTLD_NOW | RTLD_LOCAL);
    CHECK(plugin != nullptr);
    printf("plugins: a plugin loads\n");

    CHECK(*look_up<int*>(plugin, "plugin_initialized") == 42);
    printf("plugins: its initializer ran, having what it needed\n");

    auto add = look_up<int (*)(int, int)>(plugin, "plugin_add");
    CHECK(add(40, 2) == 42);
    auto* value = look_up<int*>(plugin, "plugin_value");
    CHECK(*value == 41);
    *value = 99;
    CHECK(*look_up<int*>(plugin, "plugin_value") == 99);
    printf("plugins: its code runs, and its data is there\n");

    auto double_length = look_up<int (*)(char const*)>(plugin, "plugin_double_length");
    CHECK(double_length("four") == 8);
    printf("plugins: it uses the C library, and a library nobody asked for\n");

    auto call_back = look_up<int (*)(int (*)(int), int)>(plugin, "plugin_call_back");
    CHECK(call_back(times_three, 5) == 16);
    printf("plugins: it can call back into the program\n");

    // Found under it, too: what it needs is part of it, as far as dlsym()'s concerned.
    CHECK(dlsym(plugin, "plugin_helper_double") != nullptr);
    // Opened privately, it isn't anyone else's business.
    CHECK(dlsym(RTLD_DEFAULT, "plugin_add") == nullptr);
    (void)dlerror();
    printf("plugins: opened privately, its names stay its own\n");

    // By name again: the same one.
    void* again = dlopen(s_plugin, RTLD_LAZY);
    CHECK(again == plugin);
    CHECK(*value == 99);
    CHECK(dlopen(s_plugin, RTLD_NOW | RTLD_NOLOAD) == plugin);
    CHECK(dlclose(again) == 0);
    CHECK(dlclose(plugin) == 0);
    CHECK(dlopen(s_plugin, RTLD_NOW | RTLD_GLOBAL) != nullptr);
    CHECK(dlsym(RTLD_DEFAULT, "plugin_add") != nullptr);
    printf("plugins: opened for everyone, they can all find it\n");

    Dl_info info {};
    CHECK(dladdr(reinterpret_cast<void*>(add), &info) != 0);
    CHECK(info.dli_fname && strstr(info.dli_fname, "plugin-main.dylib"));
    CHECK(info.dli_sname && strcmp(info.dli_sname, "plugin_add") == 0);
    CHECK(info.dli_saddr == reinterpret_cast<void*>(add));
    printf("plugins: dladdr() knows what's where\n");

    // What's next after this program, for one that's standing in front of it.
    CHECK(dlsym(RTLD_NEXT, "printf") == reinterpret_cast<void*>(printf));
    CHECK(dlsym(RTLD_MAIN_ONLY, "printf") == nullptr);
    (void)dlerror();
    printf("plugins: RTLD_NEXT and RTLD_MAIN_ONLY\n");
}

int (*s_next_thread_value)();

void* value_in_another_thread(void*)
{
    return reinterpret_cast<void*>(static_cast<long>(s_next_thread_value()));
}

void thread_local_data()
{
    void* plugin = dlopen(s_plugin, RTLD_NOW);
    CHECK(plugin != nullptr);
    s_next_thread_value = look_up<int (*)()>(plugin, "plugin_next_thread_value");
    // This thread's, from where it starts.
    CHECK(s_next_thread_value() == 1001);
    CHECK(s_next_thread_value() == 1003);
    // A new one's own, from its start.
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, value_in_another_thread, nullptr) == 0);
    void* result = nullptr;
    pthread_join(thread, &result);
    CHECK(reinterpret_cast<long>(result) == 1001);
    CHECK(s_next_thread_value() == 1005);
    printf("plugins: thread-local data, each thread its own\n");
    CHECK(dlclose(plugin) == 0);
}

void unloading()
{
    int note = 0;
    void* plugin = dlopen(s_unload, RTLD_NOW);
    CHECK(plugin != nullptr);
    auto set_note = look_up<void (*)(int*)>(plugin, "plugin_unload_set_note");
    auto value_of = look_up<int (*)()>(plugin, "plugin_unload_value");
    CHECK(value_of() == 7);
    printf("plugins: a C++ object in one is made\n");
    set_note(&note);

    void* second = dlopen(s_unload, RTLD_NOW);
    CHECK(second == plugin);
    CHECK(dlclose(second) == 0);
    CHECK(note == 0);
    CHECK(value_of() == 7);
    printf("plugins: closed once of twice, it stays\n");

    CHECK(dlclose(plugin) == 0);
    // The destructor (100), what atexit() was given (10), and the object (1).
    CHECK(note == 111);
    printf("plugins: closed for the last time, it finishes what it has to\n");

    Dl_info info {};
    CHECK(dladdr(reinterpret_cast<void*>(value_of), &info) == 0);
    printf("plugins: and it's gone\n");

    // Back, as new.
    plugin = dlopen(s_unload, RTLD_NOW);
    CHECK(plugin != nullptr);
    CHECK(look_up<int (*)()>(plugin, "plugin_unload_value")() == 7);
    CHECK(dlclose(plugin) == 0);
    printf("plugins: and can come back\n");

    // One that needs another keeps it, until it's done.
    int helper_finished = 0;
    void* helper = dlopen(s_helper, RTLD_NOW);
    CHECK(helper != nullptr);
    *look_up<int**>(helper, "plugin_helper_note_finished") = &helper_finished;
    CHECK(dlclose(helper) == 0);
    CHECK(helper_finished == 0); // plugin-main still needs it, and has thread-local data, so it stays.
    printf("plugins: what's still needed stays\n");

    CHECK(dlclose(reinterpret_cast<void*>(0x1000)) != 0);
    CHECK(strstr(dlerror(), "invalid handle"));
    printf("plugins: dlclose() of nothing says so\n");
}

void what_cannot_be_loaded()
{
    CHECK(dlopen("/usr/tests/plugin-broken.dylib", RTLD_NOW) == nullptr);
    char const* error = dlerror();
    CHECK(error && strstr(error, "Symbol not found") && strstr(error, "a_function_nobody_has"));
    printf("plugins: one wanting what nobody has: %s\n", strstr(error, "Symbol"));
    CHECK(dlopen("/usr/tests/plugin-broken.dylib", RTLD_NOW | RTLD_NOLOAD) == nullptr);
    (void)dlerror();

    CHECK(dlopen("no-such-plugin.dylib", RTLD_NOW) == nullptr);
    error = dlerror();
    CHECK(error && strstr(error, "tried:") && strstr(error, "no such file"));
    printf("plugins: one that isn't there says where it looked\n");

    // That all went back to how it was.
    void* plugin = dlopen(s_unload, RTLD_NOW);
    CHECK(plugin != nullptr);
    CHECK(dlclose(plugin) == 0);
    printf("plugins: having failed, everything still works\n");
}

// A lot of them, one after another.
void over_and_over()
{
    for (int i = 0; i < 200; ++i) {
        void* plugin = dlopen(s_unload, RTLD_NOW);
        CHECK(plugin != nullptr);
        CHECK(look_up<int (*)()>(plugin, "plugin_unload_value")() == 7);
        CHECK(dlclose(plugin) == 0);
    }
    printf("plugins: two hundred times, without running out of anything\n");
}

// The other way: an initializer that loads another.
void from_two_threads()
{
    constexpr int count = 4;
    pthread_t threads[count];
    for (auto& thread : threads) {
        CHECK(pthread_create(&thread, nullptr, [](void*) -> void* {
            for (int i = 0; i < 50; ++i) {
                void* plugin = dlopen(s_unload, RTLD_NOW);
                if (!plugin || !dlsym(plugin, "plugin_unload_value"))
                    return reinterpret_cast<void*>(1);
                dlclose(plugin);
            }
            return nullptr;
        }, nullptr) == 0);
    }
    for (auto& thread : threads) {
        void* result = reinterpret_cast<void*>(1);
        pthread_join(thread, &result);
        CHECK(result == nullptr);
    }
    printf("plugins: from several threads at once\n");
}

}

int main()
{
    loading();
    thread_local_data();
    unloading();
    what_cannot_be_loaded();
    over_and_over();
    from_two_threads();
    printf("plugins: PASS\n");
    return 0;
}
