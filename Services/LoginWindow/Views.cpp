/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Views.h"
#include <Desktop.h>
#include <LibGfx/Gradient.h>
#include <LibGfx/Path.h>
#include <LibLocale/Locale.h>
#include <LibUI/Appearance.h>
#include <time.h>

namespace LoginWindow {

using namespace UI;
using namespace Gfx;

namespace {

bool s_still;


}

void set_still(bool still) { s_still = still; }
bool is_still() { return s_still; }

Moment now()
{
    if (s_still)
        return { 1768470060, 0 };
    time_t const seconds = time(nullptr);
    struct tm parts {};
    localtime_r(&seconds, &parts);
    return { static_cast<i64>(seconds), static_cast<i32>(parts.tm_gmtoff) };
}

Clock::Clock()
{
    tick();
}

void Clock::tick()
{
    auto const moment = now();
    auto const& locale = Locale::default_locale();
    // "Thursday, January 15", and "9:41": no AM or PM, as Sequoia shows it.
    m_date = MUST(Locale::format_date_pattern(locale, "EEEE, MMMM d"_sv, moment.seconds, moment.offset));
    m_time = MUST(Locale::format_date_pattern(locale, "h:mm"_sv, moment.seconds, moment.offset));
    set_needs_display();
}

FloatSize Clock::intrinsic_size() const
{
    auto const& t = Appearance::the();
    return { -1, t.metric("loginwindow.dateSize"_sv) * 1.4f + t.metric("loginwindow.timeSize"_sv) };
}

void Clock::draw(Graphics& graphics)
{
    auto const& t = Appearance::the();
    float const date_size = t.metric("loginwindow.dateSize"_sv);
    float const time_size = t.metric("loginwindow.timeSize"_sv);
    FontSpec const date_font { date_size, FontWeight::SemiBold };
    FontSpec const time_font { time_size, FontWeight::Bold };
    float const date_baseline = Appearance::cap_height(date_font) + date_size * 0.2f;
    float const date_width = Appearance::text_width(date_font, m_date.view());
    graphics.draw_text(m_date.view(), date_font, { (width() - date_width) / 2, date_baseline }, t.color("loginwindow.clock"_sv));
    float const time_baseline = date_size * 1.4f + Appearance::cap_height(time_font) + time_size * 0.08f;
    float const time_width = Appearance::text_width(time_font, m_time.view());
    graphics.draw_text(m_time.view(), time_font, { (width() - time_width) / 2, time_baseline }, t.color("loginwindow.clock"_sv));
}

String Clock::accessibility_label() const
{
    StringBuilder label;
    label.appendff("{}, {}", m_date.view(), m_time.view());
    return MUST(label.to_string());
}

PowerButton::PowerButton(StringView title, bool is_restart, Function<void()> action)
    : m_title(MUST(String::from_view(title)))
    , m_is_restart(is_restart)
{
    on_action = move(action);
}

FloatSize PowerButton::intrinsic_size() const
{
    auto const& t = Appearance::the();
    return { max(t.metric("loginwindow.buttonSize"_sv), Appearance::text_width(t.named_font("small"_sv), m_title.view())), t.metric("loginwindow.buttonSize"_sv) + 18 };
}

void PowerButton::draw(Graphics& graphics)
{
    auto const& t = Appearance::the();
    float size = t.metric("loginwindow.buttonSize"_sv);
    FloatPoint center { width() / 2, size / 2 };
    graphics.fill_circle(center, size / 2, t.color("loginwindow.fieldFill"_sv).with_opacity(is_pressed() ? 1 : 0.7f));
    auto glyph = t.color("loginwindow.text"_sv);
    StrokeStyle style;
    style.width = 2.5f;
    style.cap = LineCap::Round;
    Path arc;
    if (m_is_restart) {
        // Around, with an arrowhead where it ends.
        arc.arc_to(center, 9, -0.6f, 4.4f);
        graphics.painter().stroke_path(arc, style, glyph);
        FloatPoint tip { center.x + 7.5f, center.y - 8.5f };
        Path head;
        head.move_to({ tip.x - 1, tip.y - 5 });
        head.line_to({ tip.x + 1.5f, tip.y + 2 });
        head.line_to({ tip.x - 6, tip.y + 2.5f });
        head.close();
        graphics.painter().fill_path(head, glyph);
    } else {
        graphics.line({ center.x, center.y - 9 }, { center.x, center.y }, 2.5f, glyph, true);
        arc.arc_to(center, 9, -0.9f, 4.04f);
        graphics.painter().stroke_path(arc, style, glyph);
    }
    auto font = t.named_font("small"_sv);
    graphics.draw_text_centered(m_title.view(), font, { 0, size + 2, width(), 14 }, t.color("loginwindow.text"_sv));
}

UserTile::UserTile(StringView name, StringView full_name, bool logged_in, Function<void()> action)
    : m_name(MUST(String::from_view(name)))
    , m_full_name(MUST(String::from_view(full_name)))
    , m_logged_in(logged_in)
{
    on_action = move(action);
}

void UserTile::set_logged_in(bool logged_in)
{
    m_logged_in = logged_in;
    set_needs_display();
}

FloatSize UserTile::intrinsic_size() const
{
    auto const& t = Appearance::the();
    float const picture = t.metric("loginwindow.tileAvatar"_sv);
    FontSpec const font { t.metric("loginwindow.tileNameSize"_sv), FontWeight::Medium };
    return { max(picture + 28, Appearance::text_width(font, m_full_name.view()) + 8), picture + 10 + t.metric("loginwindow.tileNameSize"_sv) + 8 };
}

void UserTile::draw(Graphics& graphics)
{
    auto const& t = Appearance::the();
    float const picture = t.metric("loginwindow.tileAvatar"_sv);
    FloatRect const circle { (width() - picture) / 2, 0, picture, picture };
    Desktop::draw_monogram(graphics, circle, m_full_name.view());
    if (is_pressed() || is_hovered())
        graphics.fill_circle({ circle.x + picture / 2, picture / 2 }, picture / 2, Color::from_rgb(0xffffff, is_pressed() ? 64 : 32));
    // Logged in already: a tick, at the picture's foot, as a Mac marks them.
    if (m_logged_in) {
        float const badge = picture * 0.3f;
        FloatPoint const center { circle.x + picture - badge * 0.4f, picture - badge * 0.4f };
        graphics.fill_circle(center, badge / 2, t.color("loginwindow.badge"_sv));
        graphics.draw_symbol(Symbol::Checkmark, { center.x - badge * 0.3f, center.y - badge * 0.3f, badge * 0.6f, badge * 0.6f }, Color::from_rgb(0xffffff), 1);
    }
    FontSpec const font { t.metric("loginwindow.tileNameSize"_sv), FontWeight::Medium };
    graphics.draw_text_centered(m_full_name.view(), font, { 0, picture + 10, width(), t.metric("loginwindow.tileNameSize"_sv) + 4 }, t.color("loginwindow.text"_sv));
}

String UserTile::accessibility_value() const
{
    return MUST(String::from_view(m_logged_in ? "logged in"_sv : ""_sv));
}

Picture::Picture(float size)
    : m_size(size)
{
}

void Picture::set_full_name(StringView full_name)
{
    m_full_name = MUST(String::from_view(full_name));
    set_needs_display();
}

void Picture::draw(Graphics& graphics)
{
    Desktop::draw_monogram(graphics, { 0, 0, m_size, m_size }, m_full_name.view());
}

}
