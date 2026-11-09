/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Plugin.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// QuickViewHelper TYPE EXTENSION WIDTH HEIGHT: what's open on standard input, pictured on standard output. Whichever plugin
// claims it runs here, sandboxed: see Desktop::request_preview. A header (magic, width, height), then the pixels.

namespace {

constexpr unsigned magic = 0x31565156;

char const* const s_places[] = { "/System/Library/QuickView", "/Library/QuickView", "/usr/tests/quickview" };

using Claims = int (*)(char const*, char const*);
using Render = int (*)(int, char const*, unsigned, unsigned, VultureQuickViewPicture*);

bool write_all(void const* bytes, size_t count)
{
    auto const* at = static_cast<char const*>(bytes);
    while (count > 0) {
        ssize_t wrote = write(STDOUT_FILENO, at, count);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        at += wrote;
        count -= static_cast<size_t>(wrote);
    }
    return true;
}

Render find_plugin(char const* type, char const* extension)
{
    for (auto const* place : s_places) {
        DIR* directory = opendir(place);
        if (!directory)
            continue;
        while (auto* entry = readdir(directory)) {
            size_t const length = strlen(entry->d_name);
            if (length < 9 || strcmp(entry->d_name + length - 9, ".qvplugin") != 0)
                continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", place, entry->d_name);
            void* plugin = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (!plugin) {
                fprintf(stderr, "QuickViewHelper: %s: %s\n", path, dlerror());
                continue;
            }
            auto claims = reinterpret_cast<Claims>(dlsym(plugin, "vulture_quickview_claims"));
            auto render = reinterpret_cast<Render>(dlsym(plugin, "vulture_quickview_render"));
            if (claims && render && claims(type, extension)) {
                closedir(directory);
                return render;
            }
            dlclose(plugin);
        }
        closedir(directory);
    }
    return nullptr;
}

}

int main(int argc, char** argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: QuickViewHelper type extension width height\n");
        return 64;
    }
    char const* type = argv[1];
    char const* extension = argv[2];
    unsigned const width = static_cast<unsigned>(atoi(argv[3]));
    unsigned const height = static_cast<unsigned>(atoi(argv[4]));
    auto render = find_plugin(type, extension);
    if (!render)
        return 2;
    VultureQuickViewPicture picture {};
    int const result = render(STDIN_FILENO, type, width, height, &picture);
    if (result != 0 || !picture.pixels)
        return 3;
    unsigned const header[3] = { magic, picture.width, picture.height };
    if (!write_all(header, sizeof(header)) || !write_all(picture.pixels, static_cast<size_t>(picture.width) * picture.height * 4))
        return 4;
    return 0;
}
