/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Notifications.h"
#include "Widgets.h"
#include <Desktop.h>
#include <LibIPC/Bootstrap.h>
#include <LibJSON/JSON.h>
#include <LibUI/Accessibility.h>
#include <LibUI/Application.h>
#include <LibUI/Appearance.h>
#include <LibUI/Event.h>
#include <LibUI/Graphics.h>
#include <LibUI/Menu.h>
#include <LibUI/Motion.h>
#include <LibUI/Platform.h>
#include <LibUI/View.h>
#include <NotificationsEndpoint.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

namespace DesktopShell {

using namespace UI;
using Gfx::Color;
using Gfx::FloatPoint;
using Gfx::FloatRect;
using Gfx::IntPoint;
using Gfx::IntRect;

namespace {

bool s_still;

Appearance const& tokens()
{
    return Appearance::the();
}

String copy(StringView text)
{
    return MUST(String::from_view(text));
}

float distance(FloatPoint a, FloatPoint b)
{
    float const x = a.x - b.x;
    float const y = a.y - b.y;
    return sqrtf(x * x + y * y);
}

// An application's settings in Settings > Notifications that only what shows them needs: whether what it says is shown
// (Show previews), and whether its notifications are stacked together (Notification grouping).
struct Display {
    bool previews { true };
    bool grouped { true };
};

Display display_for(StringView application)
{
    Display display;
    Desktop::Preferences preferences("com.vulture.notifications"_sv);
    display.previews = preferences.string("previews"_sv).value_or("always"_sv) != "never"_sv;
    if (auto const* all = preferences.object("applications"_sv)) {
        if (auto const* mine = all->get_object(application)) {
            auto const previews = mine->get_string("previews"_sv).value_or("default"_sv);
            if (previews == "always"_sv)
                display.previews = true;
            else if (previews == "never"_sv)
                display.previews = false;
            display.grouped = mine->get_string("grouping"_sv).value_or("automatic"_sv) != "off"_sv;
        }
    }
    return display;
}

// An application's icon, by its name.
StringView icon_of(StringView application)
{
    static Vector<Desktop::AppInfo> s_applications = Desktop::installed_applications();
    for (auto const& info : s_applications) {
        if (info.name.view() == application)
            return info.icon.view();
    }
    return "GenericApplication"_sv;
}

String age_of(i64 when)
{
    i64 const seconds = s_still ? 0 : static_cast<i64>(time(nullptr)) - when;
    StringBuilder builder;
    if (seconds < 60)
        builder.append("now"_sv);
    else if (seconds < 3600)
        builder.appendff("{}m ago", seconds / 60);
    else if (seconds < 86400)
        builder.appendff("{}h ago", seconds / 3600);
    else
        builder.appendff("{}d ago", seconds / 86400);
    return MUST(builder.to_string());
}

// The parts of a notification that can be pressed.
enum class Part : u8 {
    None,
    Body,
    Clear,
    Close,
    Action,
    Options,
};

struct Look {
    bool hovered { false };
    // An alert: Close and its action down the right, always.
    bool persistent { false };
    // A stack's top: the others peek out under it.
    int under { 0 };
    Part pressed_part { Part::None };
};

float card_height()
{
    return tokens().metric("notifications.height"_sv);
}

FloatPoint clear_center(FloatRect card)
{
    float const inset = tokens().metric("notifications.clearInset"_sv);
    return { card.x + inset, card.y + inset };
}

// The column of an alert's buttons, or the one button a banner shows under the pointer.
FloatRect alert_column(FloatRect card)
{
    float const width = tokens().metric("notifications.alertColumn"_sv);
    return { card.x + card.width - width, card.y, width, card.height };
}

StringView action_title(Notification const& note)
{
    if (note.actions.is_empty())
        return {};
    return note.actions.size() == 1 ? note.actions[0].view() : "Options"_sv;
}

FloatRect action_pill(FloatRect card, Notification const& note)
{
    auto const& t = tokens();
    auto const font = t.named_font("notificationButton"_sv);
    float const height = t.metric("notifications.buttonHeight"_sv);
    float const width = Appearance::text_width(font, action_title(note)) + t.metric("notifications.buttonPadding"_sv) * 2;
    float const padding = t.metric("notifications.padding"_sv);
    return { card.x + card.width - padding - width, card.y + (card.height - height) / 2, width, height };
}

Part part_at(FloatRect card, Notification const& note, Look const& look, FloatPoint point)
{
    float const clear_radius = tokens().metric("notifications.clearSize"_sv) / 2;
    if (look.hovered && distance(point, clear_center(card)) <= clear_radius + 2)
        return Part::Clear;
    if (!card.contains(point))
        return Part::None;
    if (look.persistent) {
        auto const column = alert_column(card);
        if (column.contains(point))
            return point.y < column.y + column.height / 2 ? Part::Close : (note.actions.size() > 1 ? Part::Options : Part::Action);
    } else if (look.hovered && !note.actions.is_empty() && action_pill(card, note).contains(point)) {
        return note.actions.size() > 1 ? Part::Options : Part::Action;
    }
    return Part::Body;
}

// An x, as on a clear button.
void draw_cross(Graphics& graphics, FloatPoint center, float half, Color color)
{
    graphics.line({ center.x - half, center.y - half }, { center.x + half, center.y + half }, 1.5f, color, true);
    graphics.line({ center.x - half, center.y + half }, { center.x + half, center.y - half }, 1.5f, color, true);
}

void draw_clear(Graphics& graphics, FloatPoint center)
{
    auto const& t = tokens();
    float const radius = t.metric("notifications.clearSize"_sv) / 2;
    graphics.fill_circle(center, radius, t.color("notifications.clear"_sv));
    draw_cross(graphics, center, radius * 0.38f, t.color("notifications.clearMark"_sv));
}

void draw_card(Graphics& graphics, FloatRect card, Notification const& note, Look const& look)
{
    auto const& t = tokens();
    float const radius = t.metric("notifications.radius"_sv);
    // What's under it in its stack, peeking out at the bottom.
    float const peek = t.metric("notifications.peek"_sv);
    for (int i = look.under; i >= 1; --i) {
        float const inset = peek * static_cast<float>(i);
        graphics.fill_continuous({ card.x + inset, card.y + inset, card.width - inset * 2, card.height }, radius, t.color(i == 1 ? "notifications.under"_sv : "notifications.underMore"_sv));
    }
    graphics.fill_continuous(card, radius, t.color("notifications.fill"_sv));
    auto const display = display_for(note.application.view());
    float const padding = t.metric("notifications.padding"_sv);
    float const icon = t.metric("notifications.iconSize"_sv);
    int const scale = static_cast<int>(graphics.scale());
    if (auto picture = Desktop::icon_named(icon_of(note.application.view()), static_cast<int>(icon), scale))
        graphics.painter().draw_bitmap(*picture, FloatRect { card.x + padding, card.y + (card.height - icon) / 2, icon, icon });
    float const x = card.x + padding + icon + t.metric("notifications.iconGap"_sv);
    float right = card.x + card.width - padding;
    if (look.persistent) {
        auto const column = alert_column(card);
        right = column.x - padding;
        auto const line = t.color("notifications.divider"_sv);
        graphics.fill({ column.x, column.y + 8, graphics.pixel(), column.height - 16 }, line);
        graphics.fill({ column.x + 8, column.y + column.height / 2, column.width - 16, graphics.pixel() }, line);
        auto const font = t.named_font("notificationButton"_sv);
        auto const color = t.color("label"_sv);
        graphics.draw_text_centered("Close"_sv, font, { column.x, column.y, column.width, column.height / 2 }, color);
        auto const second = note.actions.is_empty() ? "Show"_sv : action_title(note);
        graphics.draw_text_centered(second, font, { column.x, column.y + column.height / 2, column.width, column.height / 2 }, color);
    }
    bool const pill = !look.persistent && look.hovered && !note.actions.is_empty();
    auto const title_font = t.named_font("headline"_sv);
    auto const body_font = t.named_font("callout"_sv);
    auto const age = age_of(note.when);
    auto const age_font = t.named_font("notificationTime"_sv);
    float const age_width = Appearance::text_width(age_font, age.view());
    float const title_line = card.y + t.metric("notifications.titleBaseline"_sv);
    float const body_line = card.y + t.metric("notifications.bodyBaseline"_sv);
    float title_right = right;
    if (pill) {
        auto const button = action_pill(card, note);
        title_right = button.x - 8;
        graphics.fill_rounded(button, button.height / 2, t.color("notifications.button"_sv));
        graphics.draw_text_centered(action_title(note), t.named_font("notificationButton"_sv), button, t.color("label"_sv));
    } else if (!look.persistent) {
        graphics.draw_text(age.view(), age_font, { right - age_width, title_line }, t.color("notifications.secondary"_sv));
        title_right = right - age_width - 6;
    }
    StringView const title = display.previews ? note.title.view() : note.application.view();
    StringView const body = display.previews ? note.body.view() : "Notification"_sv;
    graphics.draw_text_truncated(title, title_font, { x, title_line }, title_right - x, t.color("label"_sv));
    graphics.draw_text_truncated(body, body_font, { x, body_line }, (pill ? title_right : right) - x, t.color("notifications.secondary"_sv));
    if (look.hovered)
        draw_clear(graphics, clear_center(card));
}

void add_card_parts(View const& view, Vector<Accessibility::Node>& parts, FloatRect card, Notification const& note, Look const& look)
{
    auto const display = display_for(note.application.view());
    parts.append(Accessibility::part_of(view, Accessibility::Role::Group, display.previews ? note.title.view() : note.application.view(), card, display.previews ? note.body.view() : "Notification"_sv));
    float const size = tokens().metric("notifications.clearSize"_sv);
    if (look.hovered) {
        auto const center = clear_center(card);
        parts.append(Accessibility::part_of(view, Accessibility::Role::Button, "Clear"_sv, { center.x - size / 2, center.y - size / 2, size, size }));
    }
    if (look.persistent) {
        auto const column = alert_column(card);
        parts.append(Accessibility::part_of(view, Accessibility::Role::Button, "Close"_sv, { column.x, column.y, column.width, column.height / 2 }));
        parts.append(Accessibility::part_of(view, Accessibility::Role::Button, note.actions.is_empty() ? "Show"_sv : action_title(note), { column.x, column.y + column.height / 2, column.width, column.height / 2 }));
    } else if (look.hovered && !note.actions.is_empty()) {
        parts.append(Accessibility::part_of(view, Accessibility::Role::Button, action_title(note), action_pill(card, note)));
    }
}

// The menu of an alert's or a banner's actions, under Options.
void open_actions(OwnPtr<Menu>& menu, UI::Window& owner, IntPoint at, Notifications& notifications, Notification const& note)
{
    menu = adopt_own(*new Menu);
    u32 const id = note.id;
    for (auto const& action : note.actions) {
        auto const title = copy(action.view());
        menu->add(action.view(), [&notifications, id, title] { notifications.respond(id, title.view()); });
    }
    menu->open(owner, at);
}

}

void set_notifications_still(bool still)
{
    s_still = still;
}

class EventsStub final : public NotificationEventsStub {
public:
    explicit EventsStub(Notifications& owner)
        : m_owner(owner)
    {
    }

    void posted(IPC::Sender const&, u32 id, String application, String title, String body, Vector<String> actions, String style, bool sound) override
    {
        m_owner.posted(Notification { id, move(application), move(title), move(body), move(actions), static_cast<i64>(time(nullptr)) }, style.view(), sound);
    }

    void changed(IPC::Sender const&) override { m_owner.kept_changed(); }

private:
    Notifications& m_owner;
};

// ---- Banners and alerts: at the top right, newest first, under the menu bar.

class BannerView final : public View {
public:
    explicit BannerView(Notifications& owner)
        : m_owner(owner)
    {
    }

    struct Banner {
        Notification note;
        bool persistent { false };
        int timer { -1 };
    };

    [[nodiscard]] Vector<Banner> const& banners() const { return m_banners; }
    [[nodiscard]] bool is_empty() const { return m_banners.is_empty(); }

    void add(Notification note, bool persistent)
    {
        Banner banner { move(note), persistent, -1 };
        // Temporary ones go by themselves, unless the pointer's on them. (When nothing moves, they stay until something
        // makes them go.)
        if (!persistent && motion_enabled())
            banner.timer = schedule(banner.note.id);
        m_banners.insert(0, move(banner));
        while (m_banners.size() > static_cast<size_t>(tokens().metric("notifications.bannersAtMost"_sv)))
            drop(m_banners.size() - 1);
        set_needs_display();
    }

    void dismiss(u32 id)
    {
        for (size_t i = 0; i < m_banners.size(); ++i) {
            if (m_banners[i].note.id == id) {
                drop(i);
                break;
            }
        }
        m_hovered = -1;
        set_needs_display();
        m_owner.place_banners();
    }

    void clear()
    {
        while (!m_banners.is_empty())
            drop(0);
        m_hovered = -1;
    }

    [[nodiscard]] FloatRect card_rect(size_t index) const
    {
        auto const& t = tokens();
        float const pad = t.metric("notifications.windowPad"_sv);
        float const spacing = t.metric("notifications.spacing"_sv);
        return { pad, pad + (card_height() + spacing) * static_cast<float>(index), t.metric("notifications.width"_sv), card_height() };
    }

    void draw(Graphics& graphics) override
    {
        for (size_t i = 0; i < m_banners.size(); ++i)
            draw_card(graphics, card_rect(i), m_banners[i].note, look(i));
    }

    void mouse_moved(MouseEvent const& event) override
    {
        int over = -1;
        for (size_t i = 0; i < m_banners.size(); ++i) {
            auto const card = card_rect(i);
            if (card.contains(event.position) || (static_cast<int>(i) == m_hovered && distance(event.position, clear_center(card)) <= tokens().metric("notifications.clearSize"_sv) / 2 + 2))
                over = static_cast<int>(i);
        }
        if (over != m_hovered) {
            m_hovered = over;
            set_needs_display();
        }
    }

    void mouse_exited() override
    {
        if (m_hovered >= 0) {
            m_hovered = -1;
            set_needs_display();
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        for (size_t i = 0; i < m_banners.size(); ++i) {
            auto const& banner = m_banners[i];
            auto const part = part_at(card_rect(i), banner.note, look(i), event.position);
            if (part == Part::None)
                continue;
            press(i, part, event);
            return true;
        }
        return true;
    }

    Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    String accessibility_label() const override { return copy("Notification"_sv); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        for (size_t i = 0; i < m_banners.size(); ++i)
            add_card_parts(*this, parts, card_rect(i), m_banners[i].note, look(i));
    }

private:
    [[nodiscard]] Look look(size_t index) const
    {
        Look look;
        look.hovered = static_cast<int>(index) == m_hovered;
        look.persistent = m_banners[index].persistent;
        return look;
    }

    int schedule(u32 id)
    {
        auto const seconds = static_cast<u64>(tokens().metric("notifications.seconds"_sv));
        return Application::the().add_timer(seconds * 1000, false, [this, id] {
            for (size_t i = 0; i < m_banners.size(); ++i) {
                if (m_banners[i].note.id != id)
                    continue;
                m_banners[i].timer = -1;
                // Not while it's being looked at.
                if (static_cast<int>(i) == m_hovered) {
                    m_banners[i].timer = schedule(id);
                    return;
                }
                dismiss(id);
                return;
            }
        });
    }

    void drop(size_t index)
    {
        if (m_banners[index].timer >= 0)
            Application::the().cancel_timer(m_banners[index].timer);
        m_banners.remove(index);
    }

    void press(size_t index, Part part, MouseEvent const& event)
    {
        u32 const id = m_banners[index].note.id;
        switch (part) {
        case Part::Clear:
            Platform::the().defer([owner = &m_owner, id] { owner->remove(id); });
            dismiss(id);
            return;
        case Part::Close:
            dismiss(id);
            return;
        case Part::Action:
            if (m_banners[index].note.actions.is_empty()) {
                Platform::the().defer([owner = &m_owner, id] { owner->respond(id, {}); });
                return;
            } else {
                auto const action = copy(m_banners[index].note.actions[0].view());
                Platform::the().defer([owner = &m_owner, id, action] { owner->respond(id, action.view()); });
            }
            return;
        case Part::Options: {
            auto const frame = window()->frame();
            auto const card = card_rect(index);
            auto const button = m_banners[index].persistent ? alert_column(card) : action_pill(card, m_banners[index].note);
            IntPoint const at { frame.x + static_cast<int>(button.x), frame.y + static_cast<int>(button.y + button.height) };
            open_actions(m_menu, *window(), at, m_owner, m_banners[index].note);
            (void)event;
            return;
        }
        case Part::Body:
            Platform::the().defer([owner = &m_owner, id] { owner->respond(id, {}); });
            return;
        case Part::None:
            return;
        }
    }

    Notifications& m_owner;
    Vector<Banner> m_banners;
    int m_hovered { -1 };
    OwnPtr<Menu> m_menu;
};

// ---- Notification Center: each application's notifications together, then the widgets.

class CenterView final : public View {
public:
    explicit CenterView(Notifications& owner)
        : m_owner(owner)
    {
    }

    Vector<Notification> notes;
    Function<void()> on_layout_changed;

    struct Place {
        enum class Kind : u8 {
            Header,
            Card,
            Stack,
            Widget,
            EditWidgets,
        };
        Kind kind;
        FloatRect frame;
        // The notes it's about: one, or a stack's; a header's application.
        Vector<size_t> notes;
        String application;
        size_t widget { 0 };
    };

    [[nodiscard]] Vector<Place> const& places() const { return m_places; }
    [[nodiscard]] float scroll() const { return m_scroll; }

    void lay_out()
    {
        auto const& t = tokens();
        m_places.clear();
        float const pad = t.metric("notifications.windowPad"_sv);
        float const width = t.metric("notifications.width"_sv);
        float const spacing = t.metric("notifications.spacing"_sv);
        float y = pad;
        // By application, in the order of each one's newest.
        Vector<String> order;
        for (auto const& note : notes) {
            bool seen = false;
            for (auto const& name : order)
                seen = seen || name.view() == note.application.view();
            if (!seen)
                order.append(copy(note.application.view()));
        }
        for (auto const& application : order) {
            Vector<size_t> mine;
            for (size_t i = 0; i < notes.size(); ++i) {
                if (notes[i].application.view() == application.view())
                    mine.append(i);
            }
            bool const grouped = display_for(application.view()).grouped && mine.size() > 1;
            bool const expanded = grouped && is_expanded(application.view());
            if (grouped && !expanded) {
                float const under = t.metric("notifications.peek"_sv) * static_cast<float>(min(mine.size() - 1, static_cast<size_t>(2)));
                m_places.append({ Place::Kind::Stack, { pad, y, width, card_height() + under }, move(mine), copy(application.view()), 0 });
                y += card_height() + under + spacing;
                continue;
            }
            if (expanded) {
                float const header = t.metric("notifications.headerHeight"_sv);
                m_places.append({ Place::Kind::Header, { pad, y, width, header }, {}, copy(application.view()), 0 });
                y += header;
            }
            for (auto index : mine) {
                Vector<size_t> one;
                one.append(index);
                m_places.append({ Place::Kind::Card, { pad, y, width, card_height() }, move(one), copy(application.view()), 0 });
                y += card_height() + spacing;
            }
        }
        if (!notes.is_empty())
            y += t.metric("notifications.widgetsGap"_sv) - spacing;
        // Two small ones to a row; medium and large across it.
        auto const& widgets = m_owner.m_widgets.in_center();
        float const small = widget_size(WidgetSize::Small).width;
        bool right_free = false;
        float row_top = y;
        for (size_t i = 0; i < widgets.size(); ++i) {
            auto const size = widget_size(widgets[i].size);
            if (widgets[i].size == WidgetSize::Small) {
                if (right_free) {
                    m_places.append({ Place::Kind::Widget, { pad + width - small, row_top, size.width, size.height }, {}, {}, i });
                    right_free = false;
                    continue;
                }
                row_top = y;
                m_places.append({ Place::Kind::Widget, { pad, row_top, size.width, size.height }, {}, {}, i });
                right_free = true;
                y += size.height + spacing;
                continue;
            }
            right_free = false;
            m_places.append({ Place::Kind::Widget, { pad, y, size.width, size.height }, {}, {}, i });
            y += size.height + spacing;
        }
        // Edit Widgets, under them all.
        auto const font = t.named_font("notificationButton"_sv);
        float const button_width = Appearance::text_width(font, "Edit Widgets"_sv) + t.metric("notifications.editPadding"_sv) * 2;
        float const button_height = t.metric("notifications.editHeight"_sv);
        y += t.metric("notifications.editGap"_sv) - spacing;
        m_places.append({ Place::Kind::EditWidgets, { pad + (width - button_width) / 2, y, button_width, button_height }, {}, {}, 0 });
        y += button_height + pad;
        m_content_height = y;
        m_scroll = clamp(m_scroll, 0.0f, max(m_content_height - height(), 0.0f));
        if (m_hovered >= static_cast<int>(m_places.size()))
            m_hovered = -1;
        set_needs_display();
        if (on_layout_changed)
            on_layout_changed();
    }

    // Where each one is now, in the window: what has a material under it.
    [[nodiscard]] FloatRect shown(FloatRect frame) const { return { frame.x, frame.y - m_scroll, frame.width, frame.height }; }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        bool const editing = m_owner.m_widgets.is_editing_center();
        for (size_t i = 0; i < m_places.size(); ++i) {
            auto const& place = m_places[i];
            auto const frame = shown(place.frame);
            bool const hovered = static_cast<int>(i) == m_hovered;
            switch (place.kind) {
            case Place::Kind::Header: {
                auto const font = t.named_font("notificationGroup"_sv);
                graphics.draw_text_truncated(place.application.view(), font, { frame.x + 4, frame.y + frame.height - 10 }, frame.width / 2, t.color("notifications.heading"_sv));
                auto const less = show_less_rect(frame);
                graphics.fill_rounded(less, less.height / 2, t.color("notifications.headerButton"_sv));
                graphics.draw_text_centered("Show Less"_sv, t.named_font("notificationButton"_sv), less, t.color("label"_sv));
                auto const clear = header_clear_rect(frame);
                graphics.fill_circle({ clear.x + clear.width / 2, clear.y + clear.height / 2 }, clear.width / 2, t.color("notifications.headerButton"_sv));
                draw_cross(graphics, { clear.x + clear.width / 2, clear.y + clear.height / 2 }, clear.width * 0.2f, t.color("label"_sv));
                break;
            }
            case Place::Kind::Card:
            case Place::Kind::Stack: {
                Look look;
                look.hovered = hovered;
                look.under = place.kind == Place::Kind::Stack ? static_cast<int>(min(place.notes.size() - 1, static_cast<size_t>(2))) : 0;
                draw_card(graphics, { frame.x, frame.y, frame.width, card_height() }, notes[place.notes[0]], look);
                break;
            }
            case Place::Kind::Widget: {
                auto const& placed = m_owner.m_widgets.in_center()[place.widget];
                graphics.fill_continuous(frame, t.metric("widgets.radius"_sv), t.color("notifications.widget"_sv));
                m_owner.m_widgets.draw(graphics, frame, placed, true);
                if (editing) {
                    auto const center = widget_badge(frame);
                    float const radius = t.metric("widgets.badgeSize"_sv) / 2;
                    graphics.fill_circle(center, radius, t.color("widgets.badge"_sv));
                    graphics.line({ center.x - radius * 0.45f, center.y }, { center.x + radius * 0.45f, center.y }, 2, t.color("widgets.badgeMark"_sv), true);
                }
                break;
            }
            case Place::Kind::EditWidgets:
                if (editing)
                    break;
                graphics.fill_rounded(frame, frame.height / 2, t.color(hovered ? "notifications.editHover"_sv : "notifications.edit"_sv));
                graphics.draw_text_centered("Edit Widgets"_sv, t.named_font("notificationButton"_sv), frame, t.color("label"_sv));
                break;
            }
        }
    }

    void mouse_moved(MouseEvent const& event) override
    {
        int over = -1;
        for (size_t i = 0; i < m_places.size(); ++i) {
            auto const frame = shown(m_places[i].frame);
            bool const card = m_places[i].kind == Place::Kind::Card || m_places[i].kind == Place::Kind::Stack;
            if (frame.contains(event.position) || (card && static_cast<int>(i) == m_hovered && distance(event.position, clear_center(frame)) <= tokens().metric("notifications.clearSize"_sv) / 2 + 2))
                over = static_cast<int>(i);
        }
        if (over != m_hovered) {
            m_hovered = over;
            set_needs_display();
        }
    }

    void mouse_exited() override
    {
        if (m_hovered >= 0) {
            m_hovered = -1;
            set_needs_display();
        }
    }

    bool scroll_wheel(ScrollEvent const& event) override
    {
        m_scroll = clamp(m_scroll + event.delta_y, 0.0f, max(m_content_height - height(), 0.0f));
        set_needs_display();
        if (on_layout_changed)
            on_layout_changed();
        return true;
    }

    bool mouse_down(MouseEvent const& event) override
    {
        auto const frame_of_window = window()->frame();
        IntPoint const screen { frame_of_window.x + static_cast<int>(event.window_position.x), frame_of_window.y + static_cast<int>(event.window_position.y) };
        for (size_t i = 0; i < m_places.size(); ++i) {
            auto const& place = m_places[i];
            auto const frame = shown(place.frame);
            bool const hovered = static_cast<int>(i) == m_hovered;
            switch (place.kind) {
            case Place::Kind::Header:
                if (show_less_rect(frame).contains(event.position)) {
                    set_expanded(place.application.view(), false);
                    return true;
                }
                if (header_clear_rect(frame).contains(event.position)) {
                    auto const application = copy(place.application.view());
                    Platform::the().defer([owner = &m_owner, application] { owner->clear_application(application.view()); });
                    return true;
                }
                continue;
            case Place::Kind::Card:
            case Place::Kind::Stack: {
                Look look;
                look.hovered = hovered;
                auto const& note = notes[place.notes[0]];
                auto const part = part_at({ frame.x, frame.y, frame.width, card_height() }, note, look, event.position);
                if (part == Part::None && !frame.contains(event.position))
                    continue;
                press(place, part, frame, screen);
                return true;
            }
            case Place::Kind::Widget:
                if (!frame.contains(event.position))
                    continue;
                if (event.is_right_click()) {
                    m_owner.m_widgets.open_center_menu(*window(), screen, place.widget);
                    return true;
                }
                if (m_owner.m_widgets.is_editing_center() && distance(event.position, widget_badge(frame)) <= tokens().metric("widgets.badgeSize"_sv) / 2 + 2) {
                    Platform::the().defer([owner = &m_owner, index = place.widget] { owner->m_widgets.remove_from_center(index); });
                    return true;
                }
                return true;
            case Place::Kind::EditWidgets:
                if (!frame.contains(event.position) || m_owner.m_widgets.is_editing_center())
                    continue;
                Platform::the().defer([owner = &m_owner] { owner->m_widgets.edit(true); });
                return true;
            }
        }
        // Anywhere else: it goes, unless widgets are being chosen for it.
        if (!m_owner.m_widgets.is_editing_center())
            Platform::the().defer([owner = &m_owner] { owner->hide_center(); });
        return true;
    }

    Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    String accessibility_label() const override { return copy("Notification Center"_sv); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        bool const editing = m_owner.m_widgets.is_editing_center();
        for (size_t i = 0; i < m_places.size(); ++i) {
            auto const& place = m_places[i];
            auto const frame = shown(place.frame);
            switch (place.kind) {
            case Place::Kind::Header:
                parts.append(Accessibility::part_of(*this, Accessibility::Role::StaticText, place.application.view(), frame));
                parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, "Show Less"_sv, show_less_rect(frame)));
                parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, "Clear All"_sv, header_clear_rect(frame)));
                break;
            case Place::Kind::Card: {
                Look look;
                look.hovered = static_cast<int>(i) == m_hovered;
                add_card_parts(*this, parts, frame, notes[place.notes[0]], look);
                break;
            }
            case Place::Kind::Stack: {
                StringBuilder value;
                value.appendff("{} notifications", place.notes.size());
                parts.append(Accessibility::part_of(*this, Accessibility::Role::Group, place.application.view(), frame, value.view()));
                if (static_cast<int>(i) == m_hovered) {
                    float const size = tokens().metric("notifications.clearSize"_sv);
                    auto const center = clear_center(frame);
                    parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, "Clear"_sv, { center.x - size / 2, center.y - size / 2, size, size }));
                }
                break;
            }
            case Place::Kind::Widget: {
                auto const* kind = m_owner.m_widgets.kind(m_owner.m_widgets.in_center()[place.widget].kind.view());
                if (!kind)
                    break;
                parts.append(Accessibility::part_of(*this, Accessibility::Role::Group, kind->name(), frame, kind->summary().view()));
                if (editing) {
                    float const size = tokens().metric("widgets.badgeSize"_sv);
                    auto const center = widget_badge(frame);
                    parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, "Remove Widget"_sv, { center.x - size / 2, center.y - size / 2, size, size }));
                }
                break;
            }
            case Place::Kind::EditWidgets:
                if (!editing)
                    parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, "Edit Widgets"_sv, frame));
                break;
            }
        }
    }

    void forget_expanded() { m_expanded.clear(); }

    bool accepts_first_responder() const override { return true; }
    bool shows_focus_ring() const override { return false; }
    bool key_down(KeyEvent const& event) override
    {
        if (event.code != KeyEscape)
            return false;
        Platform::the().defer([owner = &m_owner] { owner->hide_center(); });
        return true;
    }

private:
    [[nodiscard]] bool is_expanded(StringView application) const
    {
        for (auto const& name : m_expanded) {
            if (name.view() == application)
                return true;
        }
        return false;
    }

    void set_expanded(StringView application, bool expanded)
    {
        for (size_t i = 0; i < m_expanded.size(); ++i) {
            if (m_expanded[i].view() == application) {
                m_expanded.remove(i);
                break;
            }
        }
        if (expanded)
            m_expanded.append(copy(application));
        m_hovered = -1;
        lay_out();
    }

    [[nodiscard]] static FloatRect header_clear_rect(FloatRect header)
    {
        float const size = tokens().metric("notifications.headerButtonHeight"_sv);
        return { header.x + header.width - size, header.y + header.height - size - 6, size, size };
    }

    [[nodiscard]] static FloatRect show_less_rect(FloatRect header)
    {
        auto const& t = tokens();
        float const height = t.metric("notifications.headerButtonHeight"_sv);
        float const width = Appearance::text_width(t.named_font("notificationButton"_sv), "Show Less"_sv) + t.metric("notifications.buttonPadding"_sv) * 2;
        auto const clear = header_clear_rect(header);
        return { clear.x - 8 - width, clear.y, width, height };
    }

    [[nodiscard]] static FloatPoint widget_badge(FloatRect frame)
    {
        float const inset = tokens().metric("widgets.badgeInset"_sv);
        return { frame.x + inset, frame.y + inset };
    }

    void press(Place const& place, Part part, FloatRect frame, IntPoint screen)
    {
        auto const& note = notes[place.notes[0]];
        u32 const id = note.id;
        if (place.kind == Place::Kind::Stack) {
            if (part == Part::Clear) {
                auto const application = copy(place.application.view());
                Platform::the().defer([owner = &m_owner, application] { owner->clear_application(application.view()); });
                return;
            }
            // A stack opens.
            set_expanded(place.application.view(), true);
            return;
        }
        switch (part) {
        case Part::Clear:
            Platform::the().defer([owner = &m_owner, id] { owner->remove(id); });
            return;
        case Part::Action: {
            auto const action = copy(note.actions[0].view());
            Platform::the().defer([owner = &m_owner, id, action] { owner->respond(id, action.view()); });
            return;
        }
        case Part::Options: {
            auto const pill = action_pill({ frame.x, frame.y, frame.width, card_height() }, note);
            auto const window_frame = window()->frame();
            open_actions(m_menu, *window(), { window_frame.x + static_cast<int>(pill.x), window_frame.y + static_cast<int>(pill.y + pill.height) }, m_owner, note);
            (void)screen;
            return;
        }
        case Part::Body:
        case Part::Close:
        case Part::None:
            Platform::the().defer([owner = &m_owner, id] { owner->respond(id, {}); });
            return;
        }
    }

    Notifications& m_owner;
    Vector<Place> m_places;
    Vector<String> m_expanded;
    int m_hovered { -1 };
    float m_scroll { 0 };
    float m_content_height { 0 };
    OwnPtr<Menu> m_menu;
};

// ---- Notifications

Notifications::Notifications(Widgets& widgets)
    : m_widgets(widgets)
{
    auto const& t = tokens();
    auto const screen = Platform::the().screen_size();
    int const bar = static_cast<int>(t.metric("menubar.height"_sv));
    int const pad = static_cast<int>(t.metric("notifications.windowPad"_sv));
    int const width = static_cast<int>(t.metric("notifications.width"_sv));
    int const margin = static_cast<int>(t.metric("notifications.margin"_sv));
    int const left = screen.width - margin - width - pad;

    m_banner_window = adopt_own(*new UI::Window("Notification"_sv, { left, bar + margin - pad, width + pad * 2, 100 }, WS::WindowBare | WS::WindowPopup | WS::WindowSticky));
    m_banner_window->set_transparent(true);
    m_banners = &m_banner_window->root_view().add<BannerView>(*this);

    m_center_window = adopt_own(*new UI::Window("Notification Center"_sv, { left, bar, width + pad * 2, screen.height - bar }, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
    m_center_window->set_transparent(true);
    m_center_window->backend().set_level(WS::LevelPopup);
    m_center = &m_center_window->root_view().add<CenterView>(*this);
    m_center->set_frame({ 0, static_cast<float>(margin - pad), static_cast<float>(width + pad * 2), static_cast<float>(screen.height - bar - margin + pad) });
    m_center->on_layout_changed = [this] {
        // A material under each card and widget, where it is now.
        Vector<MaterialRegion> materials;
        int const radius = static_cast<int>(tokens().metric("notifications.radius"_sv));
        int const widget_radius = static_cast<int>(tokens().metric("widgets.radius"_sv));
        float const top = m_center->frame().y;
        for (auto const& place : m_center->places()) {
            auto const frame = m_center->shown(place.frame);
            IntRect const rect { static_cast<int>(frame.x), static_cast<int>(frame.y + top), static_cast<int>(frame.width), static_cast<int>(frame.height) };
            switch (place.kind) {
            case CenterView::Place::Kind::Card:
                materials.append({ rect, WS::Material::Popover, radius });
                break;
            case CenterView::Place::Kind::Stack: {
                // The card, and those under it, each its own shape.
                int const peek = static_cast<int>(tokens().metric("notifications.peek"_sv));
                int const height = static_cast<int>(card_height());
                int const under = static_cast<int>(min(place.notes.size() - 1, static_cast<size_t>(2)));
                for (int i = under; i >= 1; --i)
                    materials.append({ { rect.x + peek * i, rect.y + peek * i, rect.width - peek * i * 2, height }, WS::Material::Popover, radius });
                materials.append({ { rect.x, rect.y, rect.width, height }, WS::Material::Popover, radius });
                break;
            }
            case CenterView::Place::Kind::Widget:
                materials.append({ rect, WS::Material::Popover, widget_radius });
                break;
            case CenterView::Place::Kind::EditWidgets:
                materials.append({ rect, WS::Material::Popover, rect.height / 2 });
                break;
            case CenterView::Place::Kind::Header:
                break;
            }
        }
        m_center_window->set_materials(materials.span());
    };
    m_center_window->on_key_change = [this](bool key) {
        if (!key && m_center_shown && !m_widgets.is_editing_center())
            Platform::the().defer([this] { hide_center(); });
    };
    m_widgets.on_center_changed = [this] {
        if (m_center_shown)
            m_center->lay_out();
    };
    // Back from the gallery, it has the keyboard again.
    m_widgets.on_editing_done = [this](bool for_center) {
        if (for_center && m_center_shown)
            m_center_window->backend().activate();
    };
    m_widgets.on_center_tick = [this] {
        if (m_center_shown)
            m_center->set_needs_display();
    };

    auto port = IPC::look_up("com.vulture.notificationd"_sv);
    if (port.is_error())
        return;
    m_service = adopt_own(*new NotificationServiceProxy(port.release_value()));
    auto pair = IPC::create_port();
    if (pair.is_error())
        return;
    auto ports = pair.release_value();
    m_events = adopt_own(*new EventsStub(*this));
    m_listener = adopt_own(*new IPC::Listener(Application::the().event_loop(), move(ports.receive), [this](IPC::ReceivedMessage& message) { (void)m_events->handle(message); }));
    (void)m_service->subscribe(move(ports.send));
}

Notifications::~Notifications() = default;

void Notifications::posted(Notification note, StringView style, bool sound)
{
    // Kept, but not shown, while a Focus is on or the application wants none.
    if ((is_quiet && is_quiet()) || style == "none"_sv)
        return;
    auto const title = copy(note.title.view());
    m_banners->add(move(note), style == "persistent"_sv);
    place_banners();
    if (sound)
        Application::the().play_sound("notification"_sv);
    printf("notification: %s\n", title.characters());
    fflush(stdout);
}

void Notifications::place_banners()
{
    if (m_banners->is_empty()) {
        m_banner_window->hide();
        return;
    }
    auto const& t = tokens();
    int const pad = static_cast<int>(t.metric("notifications.windowPad"_sv));
    auto const last = m_banners->card_rect(m_banners->banners().size() - 1);
    auto frame = m_banner_window->frame();
    frame.height = static_cast<int>(last.y + last.height) + pad;
    m_banner_window->set_frame(frame);
    m_banners->set_frame({ 0, 0, static_cast<float>(frame.width), static_cast<float>(frame.height) });
    Vector<MaterialRegion> materials;
    int const radius = static_cast<int>(t.metric("notifications.radius"_sv));
    for (size_t i = 0; i < m_banners->banners().size(); ++i) {
        auto const card = m_banners->card_rect(i);
        materials.append({ { static_cast<int>(card.x), static_cast<int>(card.y), static_cast<int>(card.width), static_cast<int>(card.height) }, WS::Material::Popover, radius });
    }
    m_banner_window->set_materials(materials.span());
    m_banner_window->show();
}

void Notifications::hide_banners()
{
    m_banners->clear();
    m_banner_window->hide();
}

void Notifications::reload()
{
    m_center->notes.clear();
    if (!m_service)
        return;
    auto answer = m_service->all();
    if (answer.is_error())
        return;
    auto json = JSON::parse(answer.value().view());
    if (json.is_error() || !json.value().is_array())
        return;
    for (size_t i = 0; i < json.value().size(); ++i) {
        auto const& one = json.value().at(i);
        Notification note;
        note.id = static_cast<u32>(one.get_number("id"_sv).value_or(0));
        note.application = copy(one.get_string("application"_sv).value_or(StringView {}));
        note.title = copy(one.get_string("title"_sv).value_or(StringView {}));
        note.body = copy(one.get_string("body"_sv).value_or(StringView {}));
        note.when = static_cast<i64>(one.get_number("when"_sv).value_or(0));
        if (auto const* actions = one.get_array("actions"_sv)) {
            for (size_t a = 0; a < actions->size(); ++a) {
                if (actions->at(a).is_string())
                    note.actions.append(copy(actions->at(a).as_string()));
            }
        }
        m_center->notes.append(move(note));
    }
    m_center->lay_out();
}

void Notifications::kept_changed()
{
    if (m_center_shown)
        reload();
}

void Notifications::toggle_center()
{
    if (m_center_shown) {
        hide_center();
        return;
    }
    hide_banners();
    m_center_shown = true;
    m_center->forget_expanded();
    reload();
    m_center_window->show();
    m_center_window->backend().activate();
    m_center_window->make_first_responder(m_center);
    printf("notifications: center shown\n");
    fflush(stdout);
}

void Notifications::hide_center()
{
    if (!m_center_shown)
        return;
    m_center_shown = false;
    if (m_widgets.is_editing_center())
        m_widgets.finish_editing();
    m_center_window->hide();
}

bool Notifications::center_shown() const
{
    return m_center_shown;
}

void Notifications::widgets_changed()
{
    if (m_center_shown)
        m_center->lay_out();
}

void Notifications::respond(u32 id, StringView action)
{
    String application;
    for (auto const& banner : m_banners->banners()) {
        if (banner.note.id == id)
            application = copy(banner.note.application.view());
    }
    for (auto const& note : m_center->notes) {
        if (note.id == id)
            application = copy(note.application.view());
    }
    if (m_service)
        (void)m_service->respond(id, action);
    m_banners->dismiss(id);
    printf("notification: responded %u %.*s\n", id, static_cast<int>(action.length()), action.characters_without_null_termination());
    fflush(stdout);
    // The notification itself, clicked: its application comes forward, as on a Mac.
    if (action.is_empty() && !application.is_empty()) {
        hide_center();
        open_application(application.view());
    }
}

void Notifications::remove(u32 id)
{
    if (m_service)
        (void)m_service->remove(id);
    m_banners->dismiss(id);
}

void Notifications::clear_application(StringView application)
{
    if (m_service)
        (void)m_service->clear_application(application);
}

void Notifications::open_application(StringView application)
{
    for (auto const& info : Desktop::installed_applications()) {
        if (info.name.view() == application) {
            (void)Desktop::open_path(info.bundle_path.view());
            return;
        }
    }
}

}
