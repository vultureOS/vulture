/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibIPC/Listener.h>
#include <LibUI/Window.h>

class NotificationServiceProxy;

namespace DesktopShell {

class BannerView;
class CenterView;
class Widgets;
class EventsStub;

// For screenshots that are compared: every notification was posted just now.
void set_notifications_still(bool);

struct Notification {
    u32 id { 0 };
    String application;
    String title;
    String body;
    Vector<String> actions;
    i64 when { 0 };
};

// Notifications as a Mac shows them: banners at the top right that go by themselves, alerts that stay until they're
// answered, and Notification Center, with each application's together in a stack that opens, and the widgets under them.
class Notifications {
public:
    explicit Notifications(Widgets&);
    ~Notifications();

    // With a Focus on, banners wait: what came is in Notification Center.
    Function<bool()> is_quiet;

    void toggle_center();
    void hide_center();
    [[nodiscard]] bool center_shown() const;
    void hide_banners();
    // Its widgets have changed.
    void widgets_changed();

    // From the service, and from what's shown.
    void posted(Notification, StringView style, bool sound);
    void kept_changed();
    void respond(u32 id, StringView action);
    void remove(u32 id);
    void clear_application(StringView application);

private:
    friend class BannerView;
    friend class CenterView;

    void reload();
    void place_banners();
    void open_application(StringView);

    Widgets& m_widgets;
    OwnPtr<NotificationServiceProxy> m_service;
    OwnPtr<EventsStub> m_events;
    OwnPtr<IPC::Listener> m_listener;

    OwnPtr<UI::Window> m_banner_window;
    BannerView* m_banners { nullptr };
    OwnPtr<UI::Window> m_center_window;
    CenterView* m_center { nullptr };
    bool m_center_shown { false };
};

}
