/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Compositor.h"
#include "Screen.h"
#include "Server.h"
#include "Theme.h"
#include <LibCore/EventLoop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibText/Typeface.h>
#include <errno.h>
#include <fcntl.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// PranaWindowServer: every window on the screen. Programs draw their windows in
// memory they share with it (LibWindow), and it composes them onto the display,
// with shadows, rounded corners and translucent materials, and sends the keyboard
// and the pointer to whichever they belong to.
//
//   WindowServer [--appearance Light|Dark] [--theme FILE] [--no-input]

using namespace WindowServer;

namespace {

ErrorOr<Vector<u8>> read_whole_file(char const* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    Vector<u8> bytes;
    u8 chunk[65536];
    ssize_t got;
    while ((got = read(fd, chunk, sizeof(chunk))) > 0) {
        size_t at = bytes.size();
        TRY(bytes.try_resize(at + static_cast<size_t>(got)));
        memcpy(bytes.data() + at, chunk, static_cast<size_t>(got));
    }
    close(fd);
    return bytes;
}

}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    char const* theme_path = "/System/Library/Themes/Light.json";
    char const* appearance = "Light";
    bool with_input = true;
    bool boot_screen = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            theme_path = argv[++i];
        } else if (strcmp(argv[i], "--appearance") == 0 && i + 1 < argc) {
            appearance = argv[++i];
            theme_path = strcmp(appearance, "Dark") == 0 ? "/System/Library/Themes/Dark.json" : "/System/Library/Themes/Light.json";
        } else if (strcmp(argv[i], "--no-input") == 0) {
            with_input = false;
        } else if (strcmp(argv[i], "--still") == 0) {
            // Tests': the moment every screenshot has (9:41), for what changes with the time of day.
            WindowServer::Server::set_still(true);
        } else if (strcmp(argv[i], "--boot-screen") == 0) {
            boot_screen = true;
        } else {
            fprintf(stderr, "usage: WindowServer [--appearance Light|Dark] [--theme FILE] [--no-input] [--boot-screen]\n");
            return 64;
        }
    }
    os_log_t log = os_log_create("com.vulture.windowserver", "server");

    auto theme = load_theme(StringView(theme_path));
    if (theme.is_error()) {
        fprintf(stderr, "WindowServer: %s: %s\n", theme_path, strerror(theme.error().is_errno() ? theme.error().code() : EINVAL));
        return 1;
    }
    auto screen = Screen::open();
    if (screen.is_error()) {
        fprintf(stderr, "WindowServer: /dev/gpu0: %s\n", strerror(screen.error().code()));
        return 1;
    }
    auto font_file = read_whole_file("/System/Library/Fonts/Inter-Bold.ttf");
    if (font_file.is_error()) {
        fprintf(stderr, "WindowServer: no system font\n");
        return 1;
    }
    auto title_face = Text::Typeface::parse(font_file.release_value());
    if (title_face.is_error()) {
        fprintf(stderr, "WindowServer: the system font is damaged\n");
        return 1;
    }
    auto port = IPC::check_in("com.vulture.windowserver"_sv);
    if (port.is_error()) {
        fprintf(stderr, "WindowServer: check_in: %s\n", strerror(port.error().code()));
        return 1;
    }

    Core::EventLoop loop;
    Vector<ServerWindow*> windows;
    Compositor compositor(*screen.value(), theme.value(), windows);
    if (auto initialized = compositor.initialize(); initialized.is_error()) {
        fprintf(stderr, "WindowServer: out of memory\n");
        return 1;
    }
    Server server(loop, *screen.value(), theme.value(), compositor, windows, title_face.release_value(), StringView(appearance));
    server.set_up_cursor();
    if (with_input) {
        int input = open("/dev/input", O_RDONLY | O_CLOEXEC);
        if (input < 0)
            fprintf(stderr, "WindowServer: /dev/input: %s\n", strerror(errno));
        else
            server.watch_input(input);
    }
    IPC::Listener listener(loop, port.release_value(), [&](IPC::ReceivedMessage& message) { (void)server.handle(message); });
    auto pixels = screen.value()->pixels();
    os_log(log, "Starting: %dx%d at %dx", pixels.width, pixels.height, screen.value()->scale());
    server.start();
    if (boot_screen)
        server.show_boot_screen();
    return loop.exec();
}
