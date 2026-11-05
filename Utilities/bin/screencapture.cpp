/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <stdlib.h>
#include <LibCore/EventLoop.h>
#include <LibWindow/Window.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/vulture.h>
#include <unistd.h>

// screencapture [-x] [-D display] [-R x,y,width,height] [-V seconds] file: a picture of the screen, as on a Mac: all of it, one display
// of it (from 1), or a part of it, in points. -V records, for that long: a file for each frame, FILE-0001.ppm and so on, as there's nothing
// to put them in until there are videos.

namespace {

int record(Window::Connection& connection, Core::EventLoop& loop, Gfx::IntRect area, int seconds, char const* file)
{
    int const scale = connection.scale();
    size_t const size = static_cast<size_t>(area.width * scale) * static_cast<size_t>(area.height * scale) * 4;
    int const fd = vulture_shared_memory_create(size, O_CLOEXEC);
    if (fd < 0) {
        perror("screencapture");
        return 1;
    }
    void* memory = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) {
        perror("screencapture");
        return 1;
    }
    (void)connection.ask_to_look_at_screen();
    auto answer = connection.server().start_recording(IPC::Port(fd), area.x, area.y, area.width, area.height);
    if (answer.is_error() || answer.value() != 0) {
        fprintf(stderr, "screencapture: not allowed to record the screen\n");
        return 1;
    }
    int frames = 0;
    connection.on_recorded_frame = [&](i32 width, i32 height, u64) {
        ++frames;
        // Not every one: there's no codec to keep up with it, and what's wanted is to know that it works.
        if (frames <= 10) {
            char name[512];
            snprintf(name, sizeof(name), "%s-%04d.ppm", file, frames);
            FILE* out = fopen(name, "wb");
            if (out) {
                fprintf(out, "P6\n%d %d\n255\n", width, height);
                auto const* pixels = static_cast<u8 const*>(memory);
                for (int i = 0; i < width * height; ++i) {
                    u8 const rgb[3] = { pixels[i * 4 + 2], pixels[i * 4 + 1], pixels[i * 4] };
                    fwrite(rgb, 1, 3, out);
                }
                fclose(out);
            }
        }
        (void)connection.server().recording_ready();
    };
    bool done = false;
    loop.add_timer(static_cast<u64>(seconds) * 1000, false, [&] { done = true; });
    while (!done)
        loop.pump();
    (void)connection.server().stop_recording();
    printf("%d frames\n", frames);
    return 0;
}

}

int main(int argc, char** argv)
{
    char const* file = nullptr;
    int display_number = 0;
    int seconds = 0;
    Gfx::IntRect area;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-x") == 0)
            continue;
        if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
            display_number = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-V") == 0 && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-R") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%d,%d", &area.x, &area.y, &area.width, &area.height) != 4) {
                fprintf(stderr, "screencapture: -R x,y,width,height\n");
                return 1;
            }
        } else if (argv[i][0] != '-') {
            file = argv[i];
        } else {
            file = nullptr;
            break;
        }
    }
    if (!file) {
        fprintf(stderr, "usage: screencapture [-x] [-D display] [-R x,y,width,height] [-V seconds] file\n");
        return 1;
    }
    Core::EventLoop loop;
    auto connection_or_error = Window::Connection::connect(loop);
    if (connection_or_error.is_error()) {
        fprintf(stderr, "screencapture: there's no window server\n");
        return 1;
    }
    auto connection = connection_or_error.release_value();
    auto const displays = connection->displays();
    if (display_number > 0) {
        if (static_cast<size_t>(display_number) > displays.size()) {
            fprintf(stderr, "screencapture: there's no display %d\n", display_number);
            return 1;
        }
        auto const& display = displays[static_cast<size_t>(display_number - 1)];
        area = { display.x, display.y, display.width, display.height };
    }
    if (seconds > 0) {
        if (area.width == 0)
            area = { displays[0].x, displays[0].y, displays[0].width, displays[0].height };
        return record(*connection, loop, area, seconds, file);
    }
    auto png = connection->capture_screen(area);
    if (png.is_error()) {
        fprintf(stderr, "screencapture: not allowed to look at the screen\n");
        return 1;
    }
    int const out = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        perror(file);
        return 1;
    }
    if (write(out, png.value().data(), png.value().size()) != static_cast<ssize_t>(png.value().size())) {
        perror(file);
        return 1;
    }
    close(out);
    return 0;
}
