/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Application.h>
#include <LibUI/Controls.h>
#include <LibUI/Layout.h>
#include <Kernel/API/POSIX/privacy.h>
#include <LibIPC/Bootstrap.h>
#include <PrivacyEndpoint.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// What a package is tested with: an application, delivered over the network.

using namespace UI;

int main()
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, nullptr, _IONBF, 0);
    auto application_or_error = Application::create("Hello"_sv, "com.vulture.test.Hello"_sv);
    if (!application_or_error.is_error()) {
        // Trying to answer for itself, first: this must get nowhere.
        if (auto port = IPC::look_up("com.vulture.privacyd"_sv); !port.is_error()) {
            PrivacyProxy privacy(port.release_value());
            auto const self = "/Applications/Hello.app"_sv;
            (void)privacy.set_decision(self, VULTURE_PRIVACY_DOCUMENTS, true);
        }
        // What a person keeps to themselves: the first try makes the system ask them.
        char path[256];
        snprintf(path, sizeof(path), "%s/Documents", getenv("HOME") ? getenv("HOME") : "/tmp");
        int fd = open(path, O_RDONLY);
        printf("hello: documents %s\n", fd >= 0 ? "allowed" : (errno == EPERM ? "denied" : "unavailable"));
        if (fd >= 0)
            close(fd);
    }
    if (application_or_error.is_error()) {
        printf("hello: installed and running, with no window server to show it on\n");
        return 0;
    }
    auto application = application_or_error.release_value();
    application->set_main_menu(application->standard_menus());
    auto& window = application->make_window<UI::Window>("Hello"_sv, Gfx::IntRect { 0, 0, 320, 160 }, WS::WindowTitled);
    window.center_on_screen();
    auto& stack = window.set_content<StackView>(Axis::Vertical, 8, Alignment::Center, Insets::all(40));
    (void)stack.add<Label>("Hello from a package."_sv, Appearance::the().named_font("headline"_sv));
    window.show();
    printf("hello: ready\n");
    fflush(stdout);
    return application->exec();
}
