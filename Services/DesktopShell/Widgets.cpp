/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Widgets.h"
#include <Desktop.h>
#include <Kernel/API/POSIX/sysctl.h>
#include <LibGfx/Path.h>
#include <LibLocale/Locale.h>
#include <LibUI/Accessibility.h>
#include <LibUI/Application.h>
#include <LibUI/Appearance.h>
#include <LibUI/Controls.h>
#include <LibUI/Event.h>
#include <LibUI/Graphics.h>
#include <LibUI/Menu.h>
#include <LibUI/Platform.h>
#include <LibUI/TextField.h>
#include <LibUI/View.h>
#include <LibCore/EventLoop.h>
#include <math.h>
#include <notify.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace DesktopShell {

using namespace UI;
using Gfx::Color;
using Gfx::FloatPoint;
using Gfx::FloatRect;
using Gfx::FloatSize;
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

// The moment shown: when still, the one every screenshot has (Thursday, January 15 2026, 9:41).
struct Moment {
    i64 seconds { 0 };
    i32 offset { 0 };
};

Moment now()
{
    if (s_still)
        return { 1768470060, 0 };
    time_t const seconds = time(nullptr);
    struct tm parts {};
    localtime_r(&seconds, &parts);
    return { static_cast<i64>(seconds), static_cast<i32>(parts.tm_gmtoff) };
}

Locale::LocaleData const& locale()
{
    Desktop::Preferences preferences("com.vulture.desktop"_sv);
    auto const* found = Locale::find_locale(preferences.string("region"_sv).value_or("en_US"_sv));
    return found ? *found : Locale::default_locale();
}

String upper(StringView text)
{
    StringBuilder builder;
    for (char c : text)
        builder.append(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    return MUST(builder.to_string());
}

StringView c_view(char const* text)
{
    return text ? StringView(text, strlen(text)) : StringView {};
}

// What's shown on a widget, in full colour or in the desktop's one tint.
struct Ink {
    Color label;
    Color secondary;
    Color accent;
    Color red;
    Color track;

    static Ink of(bool full_color)
    {
        auto const& t = tokens();
        if (full_color)
            return { t.color("label"_sv), t.color("secondaryLabel"_sv), t.color("widgets.accent"_sv), t.color("systemRed"_sv), t.color("widgets.track"_sv) };
        auto const tint = t.color("widgets.dimmedText"_sv);
        return { tint, tint.with_opacity(0.6f), tint, tint, tint.with_opacity(0.25f) };
    }
};

// ---- What the machine is doing, for Monitor's widget: sampled every two seconds, while anything shows it.

class Usage {
public:
    static Usage& the()
    {
        static Usage usage;
        return usage;
    }

    void sample()
    {
        if (s_still) {
            m_cpu = 0.07f;
            m_memory = 0.38f;
            m_processes = 42;
            // A minute of it that's always the same.
            static constexpr float minute[] { 0.04f, 0.05f, 0.09f, 0.22f, 0.31f, 0.18f, 0.08f, 0.06f, 0.05f, 0.12f, 0.27f, 0.41f, 0.36f, 0.19f, 0.09f, 0.06f, 0.05f, 0.04f, 0.07f, 0.15f, 0.11f, 0.06f, 0.05f, 0.08f, 0.21f, 0.14f, 0.07f, 0.05f, 0.06f, 0.07f };
            m_history.clear();
            for (float one : minute)
                m_history.append(one);
            return;
        }
        size_t length = 0;
        if (sysctlbyname("kern.proc.all", nullptr, &length, nullptr, 0) != 0)
            return;
        length += 32 * sizeof(vulture_process_info);
        Vector<vulture_process_info> records;
        if (records.try_resize(length / sizeof(vulture_process_info)).is_error())
            return;
        if (sysctlbyname("kern.proc.all", records.data(), &length, nullptr, 0) != 0)
            return;
        size_t const count = length / sizeof(vulture_process_info);
        u64 total = 0;
        for (size_t i = 0; i < count; ++i)
            total += records[i].cpu_nanoseconds;
        timespec moment {};
        clock_gettime(CLOCK_MONOTONIC, &moment);
        u64 const at = static_cast<u64>(moment.tv_sec) * 1'000'000'000ull + static_cast<u64>(moment.tv_nsec);
        int processors = 1;
        size_t size = sizeof(processors);
        (void)sysctlbyname("hw.ncpu", &processors, &size, nullptr, 0);
        if (m_last_at != 0 && at > m_last_at && total >= m_last_total) {
            double const busy = static_cast<double>(total - m_last_total) / static_cast<double>(at - m_last_at) / max(processors, 1);
            m_cpu = static_cast<float>(clamp(busy, 0.0, 1.0));
            m_history.append(m_cpu);
            if (m_history.size() > 30)
                m_history.remove(0);
        }
        m_last_total = total;
        m_last_at = at;
        u64 memory = 0;
        u64 free = 0;
        size = sizeof(memory);
        (void)sysctlbyname("hw.memsize", &memory, &size, nullptr, 0);
        size = sizeof(free);
        (void)sysctlbyname("hw.freemem", &free, &size, nullptr, 0);
        m_memory = memory > 0 ? static_cast<float>(static_cast<double>(memory > free ? memory - free : 0) / static_cast<double>(memory)) : 0;
        m_processes = static_cast<int>(count);
    }

    [[nodiscard]] float cpu() const { return m_cpu; }
    [[nodiscard]] float memory() const { return m_memory; }
    [[nodiscard]] int processes() const { return m_processes; }
    [[nodiscard]] Vector<float> const& history() const { return m_history; }

private:
    float m_cpu { 0 };
    float m_memory { 0 };
    int m_processes { 0 };
    u64 m_last_total { 0 };
    u64 m_last_at { 0 };
    Vector<float> m_history;
};

String percent(float fraction)
{
    StringBuilder builder;
    builder.appendff("{}%", static_cast<int>(fraction * 100 + 0.5f));
    return MUST(builder.to_string());
}

// ---- The kinds there are: Calendar's, Clock's and Monitor's.

// The day, as a Mac's Calendar widget shows it: the weekday in red, the date large, and what's on.
void draw_day(Graphics& graphics, FloatRect area, Ink const& ink)
{
    auto const& t = tokens();
    auto const moment = now();
    auto const fields = Locale::date_fields(moment.seconds, moment.offset);
    auto const& data = locale();
    float const padding = t.metric("widgets.padding"_sv);
    auto const weekday = upper(c_view(data.days_standalone_wide[fields.weekday]));
    graphics.draw_text(weekday.view(), t.named_font("widgetHeading"_sv), { area.x + padding, area.y + padding + 11 }, ink.red);
    StringBuilder day;
    day.appendff("{}", fields.day);
    graphics.draw_text(day.view(), t.named_font("widgetDate"_sv), { area.x + padding - 2, area.y + padding + 56 }, ink.label);
    graphics.draw_text_truncated("No events today"_sv, t.named_font("widgetDetail"_sv), { area.x + padding, area.y + area.height - padding - 2 }, area.width - padding * 2, ink.secondary);
}

// The month, a week to a row, today ringed in red.
void draw_month(Graphics& graphics, FloatRect area, Ink const& ink, bool full_color)
{
    auto const& t = tokens();
    auto const moment = now();
    auto const fields = Locale::date_fields(moment.seconds, moment.offset);
    auto const& data = locale();
    float const padding = t.metric("widgets.monthPadding"_sv);
    auto const month = upper(c_view(data.months_standalone_wide[fields.month - 1]));
    graphics.draw_text(month.view(), t.named_font("widgetHeading"_sv), { area.x + padding + 2, area.y + padding + 11 }, ink.red);
    float const top = area.y + padding + 22;
    float const column = (area.width - padding * 2) / 7;
    float const row = (area.height - (top - area.y) - padding) / 7;
    auto const small = t.named_font("widgetWeekday"_sv);
    auto const number_font = t.named_font("widgetDay"_sv);
    for (int day = 0; day < 7; ++day) {
        auto const name = c_view(data.days_standalone_narrow[day]);
        graphics.draw_text_centered(name, small, { area.x + padding + column * static_cast<float>(day), top, column, row }, ink.secondary);
    }
    // The weekday of the first, and how many days the month has.
    int const first = ((fields.weekday - (fields.day - 1)) % 7 + 7) % 7;
    static constexpr int lengths[12] { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int length = lengths[fields.month - 1];
    if (fields.month == 2 && ((fields.year % 4 == 0 && fields.year % 100 != 0) || fields.year % 400 == 0))
        length = 29;
    for (int day = 1; day <= length; ++day) {
        int const place = first + day - 1;
        FloatRect const cell { area.x + padding + column * static_cast<float>(place % 7), top + row * static_cast<float>(place / 7 + 1), column, row };
        StringBuilder text;
        text.appendff("{}", day);
        if (day == fields.day) {
            float const radius = min(cell.width, cell.height) / 2 - 0.5f;
            graphics.fill_circle({ cell.x + cell.width / 2, cell.y + cell.height / 2 }, radius, ink.red);
            graphics.draw_text_centered(text.view(), number_font, cell, full_color ? tokens().color("widgets.today"_sv) : tokens().color("widgets.dimmedToday"_sv));
            continue;
        }
        graphics.draw_text_centered(text.view(), number_font, cell, ink.label);
    }
}

class DateKind final : public WidgetKind {
public:
    StringView identifier() const override { return "com.vulture.widget.calendar.date"_sv; }
    StringView application() const override { return "Calendar"_sv; }
    StringView name() const override { return "Date"_sv; }
    StringView description() const override { return "See the day and what\xE2\x80\x99s on."_sv; }
    bool comes_in(WidgetSize size) const override { return size == WidgetSize::Small; }
    void draw(Graphics& graphics, FloatRect area, WidgetSize, bool full_color) const override { draw_day(graphics, area, Ink::of(full_color)); }
    String summary() const override
    {
        auto const moment = now();
        return MUST(Locale::format_date(locale(), moment.seconds, Locale::Style::Full, Locale::Style::None, moment.offset));
    }
};

class MonthKind final : public WidgetKind {
public:
    StringView identifier() const override { return "com.vulture.widget.calendar.month"_sv; }
    StringView application() const override { return "Calendar"_sv; }
    StringView name() const override { return "Month"_sv; }
    StringView description() const override { return "See the month at a glance."_sv; }
    bool comes_in(WidgetSize size) const override { return size != WidgetSize::Large; }
    void draw(Graphics& graphics, FloatRect area, WidgetSize size, bool full_color) const override
    {
        auto const ink = Ink::of(full_color);
        if (size == WidgetSize::Small) {
            draw_month(graphics, area, ink, full_color);
            return;
        }
        // The day on the left, the month on the right.
        float const half = area.width / 2;
        draw_day(graphics, { area.x, area.y, half, area.height }, ink);
        draw_month(graphics, { area.x + half, area.y, half, area.height }, ink, full_color);
    }
    String summary() const override
    {
        auto const moment = now();
        return MUST(Locale::format_date_pattern(locale(), "MMMM y"_sv, moment.seconds, moment.offset));
    }
};

class ClockKind final : public WidgetKind {
public:
    StringView identifier() const override { return "com.vulture.widget.clock.city"_sv; }
    StringView application() const override { return "Clock"_sv; }
    StringView name() const override { return "City"_sv; }
    StringView description() const override { return "See the time where you are."_sv; }
    bool comes_in(WidgetSize size) const override { return size == WidgetSize::Small; }
    int refresh_seconds() const override { return 1; }

    void draw(Graphics& graphics, FloatRect area, WidgetSize, bool full_color) const override
    {
        auto const& t = tokens();
        auto const ink = Ink::of(full_color);
        auto const moment = now();
        auto const fields = Locale::date_fields(moment.seconds, moment.offset);
        float const inset = t.metric("widgets.clockInset"_sv);
        float const radius = min(area.width, area.height) / 2 - inset;
        FloatPoint const center { area.x + area.width / 2, area.y + area.height / 2 };
        graphics.fill_circle(center, radius, full_color ? t.color("widgets.clockFace"_sv) : t.color("widgets.dimmedFace"_sv));
        auto const numerals = t.named_font("widgetNumeral"_sv);
        float constexpr pi = 3.14159265f;
        for (int hour = 1; hour <= 12; ++hour) {
            float const angle = static_cast<float>(hour) * pi / 6;
            FloatPoint const at { center.x + sinf(angle) * radius * 0.78f, center.y - cosf(angle) * radius * 0.78f };
            StringBuilder text;
            text.appendff("{}", hour);
            graphics.draw_text_centered(text.view(), numerals, { at.x - 12, at.y - 10, 24, 20 }, full_color ? t.color("widgets.clockNumeral"_sv) : ink.label);
        }
        auto hand = [&](float fraction, float length, float width, Color color) {
            float const angle = fraction * 2 * pi;
            graphics.line(center, { center.x + sinf(angle) * length, center.y - cosf(angle) * length }, width, color, true);
        };
        float const seconds = static_cast<float>(fields.second);
        float const minutes = static_cast<float>(fields.minute) + seconds / 60;
        float const hours = static_cast<float>(fields.hour % 12) + minutes / 60;
        auto const hands = full_color ? t.color("widgets.clockHand"_sv) : ink.label;
        hand(hours / 12, radius * 0.5f, 4, hands);
        hand(minutes / 60, radius * 0.76f, 3, hands);
        auto const second_hand = full_color ? t.color("systemOrange"_sv) : ink.label;
        hand(seconds / 60, radius * 0.86f, 1.2f, second_hand);
        graphics.fill_circle(center, 3, second_hand);
        graphics.fill_circle(center, 1.2f, full_color ? t.color("widgets.clockFace"_sv) : t.color("widgets.dimmedFace"_sv));
    }

    String summary() const override
    {
        auto const moment = now();
        return MUST(Locale::format_date(locale(), moment.seconds, Locale::Style::None, Locale::Style::Short, moment.offset));
    }
};

class UsageKind final : public WidgetKind {
public:
    StringView identifier() const override { return "com.vulture.widget.monitor.usage"_sv; }
    StringView application() const override { return "Monitor"_sv; }
    StringView name() const override { return "Usage"_sv; }
    StringView description() const override { return "See how busy the computer is."_sv; }
    bool comes_in(WidgetSize size) const override { return size != WidgetSize::Large; }
    int refresh_seconds() const override { return 2; }

    void draw(Graphics& graphics, FloatRect area, WidgetSize size, bool full_color) const override
    {
        auto const& t = tokens();
        auto const ink = Ink::of(full_color);
        auto const& usage = Usage::the();
        float const padding = t.metric("widgets.padding"_sv);
        float const left = area.x + padding;
        float const width = (size == WidgetSize::Small ? area.width : area.width / 2) - padding * 2;
        auto bar = [&](float y, float fraction) {
            FloatRect const track { left, y, width, 6 };
            graphics.fill_rounded(track, 3, ink.track);
            graphics.fill_rounded({ track.x, track.y, max(track.width * fraction, 6.0f), track.height }, 3, ink.accent);
        };
        auto const heading = t.named_font("widgetHeading"_sv);
        graphics.draw_text("CPU"_sv, heading, { left, area.y + padding + 11 }, ink.secondary);
        graphics.draw_text(percent(usage.cpu()).view(), t.named_font("widgetValue"_sv), { left, area.y + padding + 46 }, ink.label);
        bar(area.y + padding + 56, usage.cpu());
        float const memory_top = area.y + padding + 88;
        graphics.draw_text("MEMORY"_sv, heading, { left, memory_top }, ink.secondary);
        auto const memory = percent(usage.memory());
        auto const detail = t.named_font("widgetDetail"_sv);
        graphics.draw_text(memory.view(), detail, { left + width - Appearance::text_width(detail, memory.view()), memory_top }, ink.label);
        bar(memory_top + 8, usage.memory());
        StringBuilder processes;
        processes.appendff("{} processes", usage.processes());
        graphics.draw_text(processes.view(), detail, { left, area.y + area.height - padding - 2 }, ink.secondary);
        if (size == WidgetSize::Small)
            return;
        // The last minute of it, on the right.
        FloatRect const graph { area.x + area.width / 2 + padding / 2, area.y + padding, area.width / 2 - padding * 1.5f, area.height - padding * 2 };
        graphics.fill_rounded(graph, 8, ink.track);
        auto const& history = usage.history();
        if (history.is_empty())
            return;
        float const step = graph.width / 29;
        Gfx::Path line;
        for (size_t i = 0; i < history.size(); ++i) {
            FloatPoint const at { graph.x + graph.width - step * static_cast<float>(history.size() - 1 - i), graph.y + graph.height - 4 - (graph.height - 8) * history[i] };
            if (i == 0)
                line.move_to(at);
            else
                line.line_to(at);
        }
        Gfx::StrokeStyle style;
        style.width = 2;
        style.cap = Gfx::LineCap::Round;
        style.join = Gfx::LineJoin::Round;
        graphics.painter().stroke_path(line, style, ink.accent);
    }

    String summary() const override
    {
        StringBuilder builder;
        builder.appendff("CPU {}, memory {}", percent(Usage::the().cpu()).view(), percent(Usage::the().memory()).view());
        return MUST(builder.to_string());
    }
};

constexpr WidgetSize s_sizes[] { WidgetSize::Small, WidgetSize::Medium, WidgetSize::Large };

Optional<WidgetSize> size_named(StringView name)
{
    if (name == "small"_sv)
        return WidgetSize::Small;
    if (name == "medium"_sv)
        return WidgetSize::Medium;
    if (name == "large"_sv)
        return WidgetSize::Large;
    return {};
}

// The circle with a minus in it, at a widget's corner while they're being edited.
void draw_remove_badge(Graphics& graphics, FloatPoint center)
{
    auto const& t = tokens();
    float const radius = t.metric("widgets.badgeSize"_sv) / 2;
    graphics.fill_circle(center, radius, t.color("widgets.badge"_sv));
    graphics.line({ center.x - radius * 0.45f, center.y }, { center.x + radius * 0.45f, center.y }, 2, t.color("widgets.badgeMark"_sv), true);
}

}

Gfx::FloatSize widget_size(WidgetSize size)
{
    auto const& t = tokens();
    float const small = t.metric("widgets.small"_sv);
    float const wide = small * 2 + t.metric("widgets.spacing"_sv);
    switch (size) {
    case WidgetSize::Small:
        return { small, small };
    case WidgetSize::Medium:
        return { wide, small };
    case WidgetSize::Large:
        return { wide, wide };
    }
    return { small, small };
}

StringView widget_size_name(WidgetSize size)
{
    switch (size) {
    case WidgetSize::Small:
        return "small"_sv;
    case WidgetSize::Medium:
        return "medium"_sv;
    case WidgetSize::Large:
        return "large"_sv;
    }
    return "small"_sv;
}

void set_widgets_still(bool still)
{
    s_still = still;
}

// ---- One on the desktop: a window of its own, over the icons, under everything else.

class DesktopWidgetView final : public View {
public:
    DesktopWidgetView(Widgets& owner, size_t index)
        : m_owner(owner)
        , m_index(index)
    {
    }

    void set_index(size_t index) { m_index = index; }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float const margin = t.metric("widgets.margin"_sv);
        auto const& placed = m_owner.m_desktop[m_index];
        auto const size = widget_size(placed.size);
        FloatRect const area { margin, margin, size.width, size.height };
        bool const full_color = m_owner.full_color_on_desktop();
        graphics.fill_continuous(area, t.metric("widgets.radius"_sv), full_color ? t.color("widgets.fill"_sv) : t.color("widgets.dimmedFill"_sv));
        m_owner.draw(graphics, area, placed, full_color);
        if (m_owner.is_editing())
            draw_remove_badge(graphics, badge_center());
    }

    bool mouse_down(MouseEvent const& event) override
    {
        auto const frame = window()->frame();
        IntPoint const screen { frame.x + static_cast<int>(event.window_position.x), frame.y + static_cast<int>(event.window_position.y) };
        if (event.is_right_click()) {
            m_owner.open_desktop_menu(m_index, screen);
            return true;
        }
        if (m_owner.is_editing() && distance(event.position, badge_center()) <= tokens().metric("widgets.badgeSize"_sv) / 2 + 2) {
            Platform::the().defer([owner = &m_owner, index = m_index] { owner->remove_from_desktop(index); });
            return true;
        }
        m_press_screen = screen;
        m_press_frame = frame.origin();
        m_dragging = true;
        return true;
    }

    void mouse_dragged(MouseEvent const& event) override
    {
        if (!m_dragging)
            return;
        auto frame = window()->frame();
        IntPoint const screen { frame.x + static_cast<int>(event.window_position.x), frame.y + static_cast<int>(event.window_position.y) };
        frame.x = m_press_frame.x + screen.x - m_press_screen.x;
        frame.y = m_press_frame.y + screen.y - m_press_screen.y;
        window()->set_frame(frame);
        m_moved = true;
    }

    void mouse_up(MouseEvent const&) override
    {
        if (!m_dragging)
            return;
        m_dragging = false;
        if (!m_moved)
            return;
        m_moved = false;
        float const margin = tokens().metric("widgets.margin"_sv);
        auto const frame = window()->frame();
        m_owner.moved(m_index, { frame.x + static_cast<int>(margin), frame.y + static_cast<int>(margin) });
    }

    Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    String accessibility_label() const override
    {
        auto const* kind = m_owner.kind(m_owner.m_desktop[m_index].kind.view());
        return copy(kind ? kind->name() : "Widget"_sv);
    }
    String accessibility_value() const override
    {
        auto const* kind = m_owner.kind(m_owner.m_desktop[m_index].kind.view());
        return kind ? kind->summary() : String {};
    }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        if (!m_owner.is_editing())
            return;
        float const size = tokens().metric("widgets.badgeSize"_sv);
        auto const center = badge_center();
        parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, "Remove Widget"_sv, { center.x - size / 2, center.y - size / 2, size, size }));
    }

private:
    [[nodiscard]] FloatPoint badge_center() const
    {
        float const margin = tokens().metric("widgets.margin"_sv);
        float const inset = tokens().metric("widgets.badgeInset"_sv);
        return { margin + inset, margin + inset };
    }

    static float distance(FloatPoint a, FloatPoint b)
    {
        float const x = a.x - b.x;
        float const y = a.y - b.y;
        return sqrtf(x * x + y * y);
    }

    Widgets& m_owner;
    size_t m_index { 0 };
    bool m_dragging { false };
    bool m_moved { false };
    IntPoint m_press_screen;
    IntPoint m_press_frame;
};

// ---- Edit Widgets: every kind, by application, at each size it comes in, to be chosen.

class GallerySearch final : public TextField {
public:
    GallerySearch()
        : TextField({}, FieldStyle::Rounded)
    {
        set_placeholder("Search Widgets"_sv);
        set_accessibility_label("Search Widgets"_sv);
    }

    Function<bool(KeyEvent const&)> on_key;

    bool key_down(KeyEvent const& event) override
    {
        if (on_key && on_key(event))
            return true;
        return TextField::key_down(event);
    }
};

class GalleryView final : public View {
public:
    explicit GalleryView(Widgets& owner)
        : m_owner(owner)
    {
        m_search = &add<GallerySearch>();
        m_search->on_change = [this] {
            m_scroll = 0;
            lay_out_previews();
            set_needs_display();
        };
        m_search->on_key = [this](KeyEvent const& event) {
            if (event.code == KeyEscape) {
                Platform::the().defer([owner = &m_owner] { owner->finish_editing(); });
                return true;
            }
            return false;
        };
        m_done = &add<Button>("Done"_sv);
        m_done->on_action = [this] { Platform::the().defer([owner = &m_owner] { owner->finish_editing(); }); };
        for (auto const& kind : m_owner.m_kinds) {
            bool known = false;
            for (auto const& name : m_applications)
                known = known || name.view() == kind->application();
            if (!known)
                m_applications.append(copy(kind->application()));
        }
        // In the order of their names, after All Widgets.
        for (size_t i = 0; i < m_applications.size(); ++i) {
            for (size_t j = i + 1; j < m_applications.size(); ++j) {
                if (m_applications[j].view() < m_applications[i].view())
                    swap(m_applications[i], m_applications[j]);
            }
        }
    }

    [[nodiscard]] TextField& search() { return *m_search; }

    void layout() override
    {
        auto const& t = tokens();
        float const padding = t.metric("widgets.galleryPadding"_sv);
        float const sidebar = t.metric("widgets.gallerySidebar"_sv);
        m_search->set_frame({ padding, padding, sidebar - padding * 2, m_search->intrinsic_size().height });
        auto const done = m_done->intrinsic_size();
        float const done_width = max(done.width, t.metric("widgets.galleryDoneWidth"_sv));
        m_done->set_frame({ width() - padding - done_width, height() - padding - done.height, done_width, done.height });
        lay_out_previews();
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        graphics.fill({ 0, 0, width(), height() }, t.color("widgets.gallery"_sv));
        graphics.fill({ 0, 0, width(), graphics.pixel() }, t.color("widgets.galleryEdge"_sv));
        float const padding = t.metric("widgets.galleryPadding"_sv);
        float const sidebar = t.metric("widgets.gallerySidebar"_sv);
        // The applications, down the left.
        auto const font = t.named_font("body"_sv);
        float const row_height = t.metric("widgets.galleryRowHeight"_sv);
        int const scale = static_cast<int>(graphics.scale());
        for (size_t row = 0; row <= m_applications.size(); ++row) {
            FloatRect const place = sidebar_row(row);
            if (static_cast<int>(row) == m_selected)
                graphics.fill_rounded(place, t.metric("widgets.gallerySelectionRadius"_sv), t.color("widgets.gallerySelection"_sv));
            float const icon = t.metric("widgets.galleryIcon"_sv);
            FloatRect const icon_place { place.x + 8, place.y + (place.height - icon) / 2, icon, icon };
            StringView const name = row == 0 ? "All Widgets"_sv : m_applications[row - 1].view();
            if (row == 0) {
                // Four squares, for all of them.
                float const half = (icon - 2) / 2;
                auto const color = t.color("secondaryLabel"_sv);
                graphics.fill_rounded({ icon_place.x, icon_place.y, half, half }, 2, color);
                graphics.fill_rounded({ icon_place.x + half + 2, icon_place.y, half, half }, 2, color);
                graphics.fill_rounded({ icon_place.x, icon_place.y + half + 2, half, half }, 2, color);
                graphics.fill_rounded({ icon_place.x + half + 2, icon_place.y + half + 2, half, half }, 2, color);
            } else if (!draw_application_icon(graphics, name, icon_place, scale)) {
                graphics.fill_rounded(icon_place, icon_place.width * 0.225f, t.color("secondaryLabel"_sv));
            }
            graphics.draw_text_truncated(name, font, { place.x + 16 + icon, place.y + (place.height + Appearance::cap_height(font)) / 2 }, place.width - icon - 24, t.color("label"_sv));
            (void)row_height;
        }
        graphics.fill({ sidebar, padding, graphics.pixel(), height() - padding * 2 }, t.color("separator"_sv));
        // The widgets, at their own size.
        graphics.save();
        graphics.clip({ sidebar + 1, 0, width() - sidebar - 1, height() });
        auto const heading = t.named_font("headline"_sv);
        auto const name_font = t.named_font("widgetGalleryName"_sv);
        auto const detail = t.named_font("small"_sv);
        for (auto const& section : m_sections)
            graphics.draw_text(section.title.view(), heading, { section.at.x, section.at.y - m_scroll }, t.color("label"_sv));
        for (size_t i = 0; i < m_previews.size(); ++i) {
            auto const& preview = m_previews[i];
            auto const* kind = m_owner.kind(preview.kind.view());
            if (!kind)
                continue;
            FloatRect const area { preview.frame.x, preview.frame.y - m_scroll, preview.frame.width, preview.frame.height };
            graphics.fill_continuous(area, t.metric("widgets.radius"_sv), t.color("widgets.fill"_sv));
            graphics.stroke_rounded(area, t.metric("widgets.radius"_sv), graphics.pixel(), t.color("widgets.galleryOutline"_sv));
            PlacedWidget const placed { copy(preview.kind.view()), preview.size, {} };
            m_owner.draw(graphics, area, placed, true);
            graphics.draw_text_truncated(kind->name(), name_font, { area.x, area.y + area.height + 18 }, area.width, t.color("label"_sv));
            graphics.draw_text_truncated(kind->description(), detail, { area.x, area.y + area.height + 34 }, area.width, t.color("secondaryLabel"_sv));
            if (static_cast<int>(i) == m_hovered) {
                // A plus at its corner: chosen, it's added.
                FloatPoint const center { area.x + t.metric("widgets.badgeInset"_sv), area.y + t.metric("widgets.badgeInset"_sv) };
                float const radius = t.metric("widgets.badgeSize"_sv) / 2;
                graphics.fill_circle(center, radius, t.color("widgets.addBadge"_sv));
                auto const mark = t.color("widgets.addMark"_sv);
                graphics.line({ center.x - radius * 0.45f, center.y }, { center.x + radius * 0.45f, center.y }, 2, mark, true);
                graphics.line({ center.x, center.y - radius * 0.45f }, { center.x, center.y + radius * 0.45f }, 2, mark, true);
            }
        }
        graphics.restore();
    }

    bool mouse_down(MouseEvent const& event) override
    {
        for (size_t row = 0; row <= m_applications.size(); ++row) {
            if (sidebar_row(row).contains(event.position)) {
                m_selected = static_cast<int>(row);
                m_scroll = 0;
                lay_out_previews();
                set_needs_display();
                return true;
            }
        }
        int const index = preview_at(event.position);
        if (index >= 0) {
            auto const& preview = m_previews[static_cast<size_t>(index)];
            auto const kind = copy(preview.kind.view());
            auto const size = preview.size;
            Platform::the().defer([owner = &m_owner, kind, size] { owner->add(kind.view(), size); });
            return true;
        }
        return true;
    }

    void mouse_moved(MouseEvent const& event) override
    {
        int const over = preview_at(event.position);
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
        return true;
    }

    Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    String accessibility_label() const override { return copy("Widgets"_sv); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        for (size_t row = 0; row <= m_applications.size(); ++row)
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Row, row == 0 ? "All Widgets"_sv : m_applications[row - 1].view(), sidebar_row(row), static_cast<int>(row) == m_selected ? "selected"_sv : StringView {}));
        for (auto const& preview : m_previews) {
            auto const* kind = m_owner.kind(preview.kind.view());
            if (!kind)
                continue;
            static constexpr StringView sizes[] { "Small"_sv, "Medium"_sv, "Large"_sv };
            StringBuilder label;
            label.appendff("{} {} ({})", kind->application(), kind->name(), sizes[static_cast<int>(preview.size)]);
            FloatRect const area { preview.frame.x, preview.frame.y - m_scroll, preview.frame.width, preview.frame.height };
            if (area.y + area.height < 0 || area.y > height())
                continue;
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Cell, label.view(), area, widget_size_name(preview.size)));
        }
    }

private:
    struct Preview {
        String kind;
        WidgetSize size;
        FloatRect frame;
    };
    struct Section {
        String title;
        FloatPoint at;
    };

    [[nodiscard]] FloatRect sidebar_row(size_t row) const
    {
        auto const& t = tokens();
        float const padding = t.metric("widgets.galleryPadding"_sv);
        float const sidebar = t.metric("widgets.gallerySidebar"_sv);
        float const top = padding + m_search->intrinsic_size().height + padding;
        float const height = t.metric("widgets.galleryRowHeight"_sv);
        return { padding / 2, top + height * static_cast<float>(row), sidebar - padding, height };
    }

    [[nodiscard]] int preview_at(FloatPoint point) const
    {
        for (size_t i = 0; i < m_previews.size(); ++i) {
            auto const& frame = m_previews[i].frame;
            if (FloatRect { frame.x, frame.y - m_scroll, frame.width, frame.height }.contains(point))
                return static_cast<int>(i);
        }
        return -1;
    }

    // The application's own icon; Calendar and Clock, whose applications come with M31, drawn here till then.
    static bool draw_application_icon(Graphics& graphics, StringView application, FloatRect place, int scale)
    {
        auto const& t = tokens();
        float const radius = place.width * 0.225f;
        if (application == "Calendar"_sv) {
            graphics.fill_continuous(place, radius, t.color("widgets.iconPaper"_sv));
            graphics.save();
            graphics.clip({ place.x, place.y, place.width, place.height * 0.3f });
            graphics.fill_continuous(place, radius, t.color("systemRed"_sv));
            graphics.restore();
            graphics.draw_text_centered("15"_sv, FontSpec { place.height * 0.5f, FontWeight::Medium, false }, { place.x, place.y + place.height * 0.3f, place.width, place.height * 0.68f }, t.color("widgets.iconInk"_sv));
            return true;
        }
        if (application == "Clock"_sv) {
            graphics.fill_continuous(place, radius, t.color("widgets.iconInk"_sv));
            FloatPoint const center { place.x + place.width / 2, place.y + place.height / 2 };
            graphics.fill_circle(center, place.width * 0.38f, t.color("widgets.iconPaper"_sv));
            graphics.line(center, { center.x, center.y - place.height * 0.28f }, 1.5f, t.color("widgets.iconInk"_sv), true);
            graphics.line(center, { center.x + place.width * 0.2f, center.y }, 1.5f, t.color("widgets.iconInk"_sv), true);
            return true;
        }
        auto picture = Desktop::icon_named(application, static_cast<int>(place.width), scale);
        if (!picture)
            return false;
        graphics.painter().draw_bitmap(*picture, place);
        return true;
    }

    [[nodiscard]] bool matches(WidgetKind const& kind) const
    {
        auto const typed = m_search->text();
        if (typed.is_empty())
            return true;
        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        auto contains = [&](StringView text) {
            for (size_t at = 0; at + typed.length() <= text.length(); ++at) {
                bool same = true;
                for (size_t i = 0; i < typed.length() && same; ++i)
                    same = lower(text[at + i]) == lower(typed[i]);
                if (same)
                    return true;
            }
            return false;
        };
        return contains(kind.name()) || contains(kind.application());
    }

    void lay_out_previews()
    {
        auto const& t = tokens();
        m_previews.clear();
        m_sections.clear();
        float const padding = t.metric("widgets.galleryPadding"_sv);
        float const left = t.metric("widgets.gallerySidebar"_sv) + padding * 2;
        float const right = width() - padding * 2;
        float const spacing = t.metric("widgets.galleryPreviewSpacing"_sv);
        float const below = t.metric("widgets.galleryCaption"_sv);
        float y = padding;
        for (size_t a = 0; a < m_applications.size(); ++a) {
            if (m_selected > 0 && static_cast<size_t>(m_selected - 1) != a)
                continue;
            bool any = false;
            float x = left;
            float row_height = 0;
            for (auto const& kind : m_owner.m_kinds) {
                if (kind->application() != m_applications[a].view() || !matches(*kind))
                    continue;
                for (auto size : s_sizes) {
                    if (!kind->comes_in(size))
                        continue;
                    if (!any) {
                        m_sections.append({ copy(m_applications[a].view()), { left, y + 14 } });
                        y += t.metric("widgets.galleryHeading"_sv);
                        any = true;
                    }
                    auto const dimensions = widget_size(size);
                    if (x + dimensions.width > right && x > left) {
                        x = left;
                        y += row_height + below + spacing;
                        row_height = 0;
                    }
                    m_previews.append({ copy(kind->identifier()), size, { x, y, dimensions.width, dimensions.height } });
                    x += dimensions.width + spacing;
                    row_height = max(row_height, dimensions.height);
                }
            }
            if (any)
                y += row_height + below + spacing;
        }
        m_content_height = y;
        if (m_hovered >= static_cast<int>(m_previews.size()))
            m_hovered = -1;
    }

    Widgets& m_owner;
    GallerySearch* m_search { nullptr };
    Button* m_done { nullptr };
    Vector<String> m_applications;
    Vector<Preview> m_previews;
    Vector<Section> m_sections;
    int m_selected { 0 };
    int m_hovered { -1 };
    float m_scroll { 0 };
    float m_content_height { 0 };
};

// ---- Widgets

Widgets::Widgets()
{
    m_kinds.append(adopt_own(*new DateKind));
    m_kinds.append(adopt_own(*new MonthKind));
    m_kinds.append(adopt_own(*new ClockKind));
    m_kinds.append(adopt_own(*new UsageKind));
    Usage::the().sample();
    load();
    show_desktop();
    if (!s_still)
        m_timer = Application::the().add_timer(1000, true, [this] { tick(); });
    // Desktop & Dock's settings for them, and Files' Edit Widgets... on the desktop.
    auto listen = [](char const* name, Function<void()> heard) {
        int descriptor = -1;
        int token = -1;
        if (notify_register_file_descriptor(name, &descriptor, 0, &token) != NOTIFY_STATUS_OK)
            return;
        auto* raw = adopt_own(*new Function<void()>(move(heard))).leak_ptr();
        (void)Application::the().event_loop().watch(descriptor, Core::EventLoop::Interest::Read, [descriptor, raw] {
            u8 bytes[4];
            (void)read(descriptor, bytes, sizeof(bytes));
            (*raw)();
        });
    };
    listen("com.vulture.widgets.preferences", [this] {
        load();
        show_desktop();
        if (on_center_changed)
            on_center_changed();
    });
    listen("com.vulture.widgets.edit", [this] { edit(false); });
}

Widgets::~Widgets()
{
    if (m_timer >= 0)
        Application::the().cancel_timer(m_timer);
}

WidgetKind const* Widgets::kind(StringView identifier) const
{
    for (auto const& kind : m_kinds) {
        if (kind->identifier() == identifier)
            return kind.ptr();
    }
    return nullptr;
}

void Widgets::draw(Graphics& graphics, FloatRect area, PlacedWidget const& placed, bool full_color) const
{
    auto const* found = kind(placed.kind.view());
    if (!found)
        return;
    graphics.save();
    graphics.clip(area);
    found->draw(graphics, area, placed.size, full_color);
    graphics.restore();
}

void Widgets::load()
{
    m_desktop.clear();
    m_center.clear();
    Desktop::Preferences preferences("com.vulture.widgets"_sv);
    m_style = copy(preferences.string("style"_sv).value_or("automatic"_sv));
    m_on_desktop = preferences.boolean("onDesktop"_sv).value_or(true);
    auto read = [&](JSON::Value const* list, Vector<PlacedWidget>& into) {
        if (!list)
            return false;
        for (size_t i = 0; i < list->size(); ++i) {
            auto const& one = list->at(i);
            auto const identifier = one.get_string("kind"_sv).value_or(StringView {});
            auto const size = size_named(one.get_string("size"_sv).value_or("small"_sv));
            auto const* found = kind(identifier);
            if (!found || !size.has_value() || !found->comes_in(*size))
                continue;
            into.append({ copy(identifier), *size, { static_cast<int>(one.get_number("x"_sv).value_or(0)), static_cast<int>(one.get_number("y"_sv).value_or(0)) } });
        }
        return true;
    };
    (void)read(preferences.array("desktop"_sv), m_desktop);
    // A new account's Notification Center has the day and the time in it.
    if (!read(preferences.array("center"_sv), m_center)) {
        m_center.append({ copy("com.vulture.widget.calendar.date"_sv), WidgetSize::Small, {} });
        m_center.append({ copy("com.vulture.widget.clock.city"_sv), WidgetSize::Small, {} });
    }
}

void Widgets::save()
{
    Desktop::Preferences preferences("com.vulture.widgets"_sv);
    auto write = [](Vector<PlacedWidget> const& list, bool with_place) {
        auto array = JSON::Value::array();
        for (auto const& placed : list) {
            auto one = JSON::Value::object();
            MUST(one.set("kind"_sv, MUST(JSON::Value::string(placed.kind.view()))));
            MUST(one.set("size"_sv, MUST(JSON::Value::string(widget_size_name(placed.size)))));
            if (with_place) {
                MUST(one.set("x"_sv, JSON::Value(static_cast<double>(placed.at.x))));
                MUST(one.set("y"_sv, JSON::Value(static_cast<double>(placed.at.y))));
            }
            MUST(array.append(move(one)));
        }
        return array;
    };
    preferences.set("desktop"_sv, write(m_desktop, true));
    preferences.set("center"_sv, write(m_center, false));
    preferences.save();
}

void Widgets::show_desktop()
{
    for (auto& window : m_desktop_windows) {
        if (window)
            window->hide();
    }
    // (Not from inside one of their own presses.)
    auto* old = new Vector<OwnPtr<UI::Window>>(move(m_desktop_windows));
    Platform::the().defer([old] { delete old; });
    m_desktop_windows.clear();
    m_desktop_views.clear();
    if (!m_on_desktop)
        return;
    auto const& t = tokens();
    int const margin = static_cast<int>(t.metric("widgets.margin"_sv));
    for (size_t i = 0; i < m_desktop.size(); ++i) {
        auto const& placed = m_desktop[i];
        auto const size = widget_size(placed.size);
        IntRect const frame { placed.at.x - margin, placed.at.y - margin, static_cast<int>(size.width) + margin * 2, static_cast<int>(size.height) + margin * 2 };
        auto const* found = kind(placed.kind.view());
        auto window = adopt_own(*new UI::Window(found ? found->name() : "Widget"_sv, frame, WS::WindowBare | WS::WindowSticky));
        window->set_transparent(true);
        window->backend().set_level(WS::LevelDesktopItems);
        MaterialRegion const material { { margin, margin, static_cast<int>(size.width), static_cast<int>(size.height) }, WS::Material::Popover, static_cast<int>(t.metric("widgets.radius"_sv)) };
        window->set_materials(Span<MaterialRegion const>(&material, 1));
        auto& view = window->root_view().add<DesktopWidgetView>(*this, i);
        view.set_frame({ 0, 0, static_cast<float>(frame.width), static_cast<float>(frame.height) });
        window->show();
        m_desktop_views.append(&view);
        m_desktop_windows.append(move(window));
    }
}

bool Widgets::full_color_on_desktop() const
{
    if (m_style.view() == "fullColor"_sv)
        return true;
    if (m_style.view() == "monochrome"_sv)
        return false;
    return m_desktop_active || m_editing;
}

void Widgets::set_desktop_active(bool active)
{
    if (active == m_desktop_active)
        return;
    m_desktop_active = active;
    for (auto* view : m_desktop_views)
        view->set_needs_display();
}

void Widgets::tick()
{
    static int s_ticks = 0;
    ++s_ticks;
    // Looking at every process is work: only while there's a Usage widget to show it, wherever it is (one in a closed
    // Notification Center keeps its history).
    auto const has_usage = [](Vector<PlacedWidget> const& placed) {
        for (auto const& widget : placed) {
            if (widget.kind.view() == "com.vulture.widget.monitor.usage"_sv)
                return true;
        }
        return false;
    };
    if (s_ticks % 2 == 0 && (has_usage(m_desktop) || has_usage(m_center)))
        Usage::the().sample();
    for (size_t i = 0; i < m_desktop_views.size() && i < m_desktop.size(); ++i) {
        auto const* found = kind(m_desktop[i].kind.view());
        if (found && s_ticks % found->refresh_seconds() == 0)
            m_desktop_views[i]->set_needs_display();
    }
    if (on_center_tick)
        on_center_tick();
}

IntPoint Widgets::free_place(WidgetSize size) const
{
    auto const& t = tokens();
    auto const screen = Platform::the().screen_size();
    float const spacing = t.metric("widgets.spacing"_sv);
    float const left = t.metric("widgets.desktopLeft"_sv);
    float const top = t.metric("menubar.height"_sv) + t.metric("widgets.desktopTop"_sv);
    auto const wanted = widget_size(size);
    float const step = t.metric("widgets.small"_sv) + spacing;
    auto overlaps = [&](FloatRect const& place) {
        for (auto const& other : m_desktop) {
            auto const other_size = widget_size(other.size);
            FloatRect const taken { static_cast<float>(other.at.x) - spacing + 1, static_cast<float>(other.at.y) - spacing + 1, other_size.width + spacing * 2 - 2, other_size.height + spacing * 2 - 2 };
            if (taken.intersects(place))
                return true;
        }
        return false;
    };
    // Down the left first, then the next column, as Sonoma puts them.
    for (float x = left; x + wanted.width <= static_cast<float>(screen.width); x += step) {
        for (float y = top; y + wanted.height <= static_cast<float>(screen.height) - t.metric("dock.windowHeight"_sv); y += step) {
            FloatRect const place { x, y, wanted.width, wanted.height };
            if (!overlaps(place))
                return { static_cast<int>(x), static_cast<int>(y) };
        }
    }
    return { static_cast<int>(left), static_cast<int>(top) };
}

void Widgets::add(StringView identifier, WidgetSize size)
{
    if (!kind(identifier))
        return;
    if (m_for_center && m_editing) {
        m_center.append({ copy(identifier), size, {} });
        save();
        if (on_center_changed)
            on_center_changed();
    } else {
        m_desktop.append({ copy(identifier), size, free_place(size) });
        save();
        show_desktop();
    }
    printf("widgets: added %.*s, %.*s\n", static_cast<int>(identifier.length()), identifier.characters_without_null_termination(), static_cast<int>(widget_size_name(size).length()), widget_size_name(size).characters_without_null_termination());
    fflush(stdout);
}

void Widgets::remove_from_desktop(size_t index)
{
    if (index >= m_desktop.size())
        return;
    m_desktop.remove(index);
    save();
    show_desktop();
    printf("widgets: removed\n");
    fflush(stdout);
}

void Widgets::remove_from_center(size_t index)
{
    if (index >= m_center.size())
        return;
    m_center.remove(index);
    save();
    if (on_center_changed)
        on_center_changed();
    printf("widgets: removed\n");
    fflush(stdout);
}

void Widgets::resize(bool center, size_t index, WidgetSize size)
{
    auto& list = center ? m_center : m_desktop;
    if (index >= list.size())
        return;
    list[index].size = size;
    save();
    if (center) {
        if (on_center_changed)
            on_center_changed();
    } else {
        show_desktop();
    }
}

void Widgets::moved(size_t index, IntPoint at)
{
    if (index >= m_desktop.size())
        return;
    m_desktop[index].at = at;
    save();
}

void Widgets::open_desktop_menu(size_t index, IntPoint screen_point)
{
    if (index >= m_desktop_windows.size())
        return;
    auto const* found = kind(m_desktop[index].kind.view());
    if (!found)
        return;
    m_menu = adopt_own(*new Menu);
    static constexpr StringView names[] { "Small"_sv, "Medium"_sv, "Large"_sv };
    for (auto size : s_sizes) {
        if (!found->comes_in(size))
            continue;
        m_menu->add(names[static_cast<int>(size)], [this, index, size] { resize(false, index, size); }).checked = m_desktop[index].size == size;
    }
    m_menu->add_separator();
    m_menu->add("Remove Widget"_sv, [this, index] { Platform::the().defer([this, index] { remove_from_desktop(index); }); });
    m_menu->add_separator();
    m_menu->add("Edit Widgets\xE2\x80\xA6"_sv, [this] { Platform::the().defer([this] { edit(false); }); });
    m_menu->open(*m_desktop_windows[index], screen_point);
}

void Widgets::open_center_menu(UI::Window& owner, IntPoint screen_point, size_t index)
{
    if (index >= m_center.size())
        return;
    auto const* found = kind(m_center[index].kind.view());
    if (!found)
        return;
    m_menu = adopt_own(*new Menu);
    static constexpr StringView names[] { "Small"_sv, "Medium"_sv, "Large"_sv };
    for (auto size : s_sizes) {
        if (!found->comes_in(size))
            continue;
        m_menu->add(names[static_cast<int>(size)], [this, index, size] { resize(true, index, size); }).checked = m_center[index].size == size;
    }
    m_menu->add_separator();
    m_menu->add("Remove Widget"_sv, [this, index] { Platform::the().defer([this, index] { remove_from_center(index); }); });
    m_menu->add_separator();
    m_menu->add("Edit Widgets\xE2\x80\xA6"_sv, [this] { Platform::the().defer([this] { edit(true); }); });
    m_menu->open(owner, screen_point);
}

void Widgets::edit(bool for_center)
{
    m_for_center = for_center;
    if (m_editing && m_gallery_window) {
        m_gallery_window->backend().activate();
        return;
    }
    m_editing = true;
    auto const& t = tokens();
    auto const screen = Platform::the().screen_size();
    int const height = static_cast<int>(t.metric("widgets.galleryHeight"_sv));
    IntRect const frame { 0, screen.height - height, screen.width, height };
    m_gallery_window = adopt_own(*new UI::Window("Widgets"_sv, frame, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
    m_gallery_window->set_transparent(true);
    m_gallery_window->backend().set_level(WS::LevelPopup);
    MaterialRegion const material { { 0, 0, frame.width, frame.height }, WS::Material::Popover, 0 };
    m_gallery_window->set_materials(Span<MaterialRegion const>(&material, 1));
    m_gallery = &m_gallery_window->root_view().add<GalleryView>(*this);
    m_gallery->set_frame({ 0, 0, static_cast<float>(frame.width), static_cast<float>(frame.height) });
    m_gallery_window->show();
    m_gallery_window->backend().activate();
    m_gallery_window->make_first_responder(&m_gallery->search());
    for (auto* view : m_desktop_views)
        view->set_needs_display();
    if (on_center_changed)
        on_center_changed();
    printf("widgets: editing%s\n", for_center ? " Notification Center" : "");
    fflush(stdout);
}

void Widgets::finish_editing()
{
    if (!m_editing)
        return;
    m_editing = false;
    if (m_gallery_window) {
        m_gallery_window->hide();
        auto* old = m_gallery_window.leak_ptr();
        Platform::the().defer([old] { delete old; });
    }
    m_gallery = nullptr;
    for (auto* view : m_desktop_views)
        view->set_needs_display();
    if (on_center_changed)
        on_center_changed();
    if (on_editing_done)
        on_editing_done(m_for_center);
    printf("widgets: done\n");
    fflush(stdout);
}

}
