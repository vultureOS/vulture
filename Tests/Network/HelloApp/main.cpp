/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Application.h>
#include <LibUI/Controls.h>
#include <LibUI/Layout.h>
#include <signal.h>
#include <stdio.h>

// What a package is tested with: an application, delivered over the network.

using namespace UI;

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto application_or_error = Application::create("Hello"_sv, "com.vulture.test.Hello"_sv);
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
