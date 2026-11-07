/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibDesktop/ScreenSaver.h>
#include <LibGfx/Gradient.h>
#include <LibGfx/Path.h>
#include <LibLocale/Locale.h>
#include <LibUI/Appearance.h>
#include <time.h>

namespace Desktop {

using namespace UI;
using namespace Gfx;

namespace {

// Each light: where it goes round (as fractions of the screen), how fast, how big, and its colour.
struct Light {
    float x;
    float y;
    float reach_x;
    float reach_y;
    float speed;
    float phase;
    float radius;
    u32 color;
};

constexpr Light s_lights[] = {
    { 0.30f, 0.35f, 0.18f, 0.12f, 0.050f, 0.0f, 0.42f, 0x3a5bd9 },
    { 0.70f, 0.40f, 0.15f, 0.16f, 0.041f, 1.7f, 0.38f, 0x9a4fd0 },
    { 0.50f, 0.70f, 0.22f, 0.10f, 0.033f, 3.1f, 0.45f, 0xd9607a },
    { 0.20f, 0.75f, 0.12f, 0.14f, 0.057f, 4.4f, 0.30f, 0x2fa3b8 },
    { 0.82f, 0.78f, 0.10f, 0.12f, 0.046f, 5.2f, 0.28f, 0xe39a4a },
};

constexpr StringView s_names[] = { "Drift"_sv, "Clock"_sv };

}

ScreenSaver::ScreenSaver(StringView name)
    : m_is_clock(name == "Clock"_sv)
{
}

Span<StringView const> ScreenSaver::names()
{
    return { s_names, array_size(s_names) };
}

void ScreenSaver::advance(double seconds)
{
    m_seconds = seconds;
    set_needs_display();
}

void ScreenSaver::set_time(i64 seconds_since_1970, i32 offset_seconds)
{
    m_has_time = true;
    m_time = seconds_since_1970;
    m_offset = offset_seconds;
    set_needs_display();
}

void ScreenSaver::draw(Graphics& graphics)
{
    draw_in(graphics, { 0, 0, width(), height() });
}

void ScreenSaver::draw_in(Graphics& graphics, FloatRect area)
{
    graphics.fill(area, Color::from_rgb(0x000000));
    float const w = area.width;
    float const h = area.height;
    if (!m_is_clock) {
        float const reach = max(w, h);
        graphics.save();
        graphics.clip(area);
        for (auto const& light : s_lights) {
            double const angle = m_seconds * light.speed * 2 * 3.14159265358979 + light.phase;
            FloatPoint const center {
                area.x + w * (light.x + light.reach_x * static_cast<float>(Base::Math::cosine(angle))),
                area.y + h * (light.y + light.reach_y * static_cast<float>(Base::Math::sine(angle * 1.3))),
            };
            float const radius = reach * light.radius;
            GradientStop const stops[] = {
                { 0, Color::from_rgb(light.color, 150) },
                { 0.5f, Color::from_rgb(light.color, 60) },
                { 1, Color::from_rgb(light.color, 0) },
            };
            Path circle;
            circle.add_circle(center, radius);
            graphics.painter().fill_path(circle, Gradient::radial(center, radius, Span<GradientStop const>(stops, 3)));
        }
        graphics.restore();
        return;
    }
    i64 seconds = m_time;
    i32 offset = m_offset;
    if (!m_has_time) {
        time_t const now = time(nullptr);
        struct tm parts {};
        localtime_r(&now, &parts);
        seconds = static_cast<i64>(now);
        offset = static_cast<i32>(parts.tm_gmtoff);
    }
    auto const text = MUST(Locale::format_date_pattern(Locale::default_locale(), "h:mm"_sv, seconds, offset));
    FontSpec const font { min(w, h) * 0.2f, FontWeight::Bold };
    float const text_width = Appearance::text_width(font, text.view());
    float const text_height = Appearance::cap_height(font);
    // A different place each minute, along a slow figure, so that nothing's burnt in.
    double const minute = static_cast<double>(seconds / 60);
    float const room_x = max(0.0f, w - text_width) / 2;
    float const room_y = max(0.0f, h - text_height) / 2;
    float const x = area.x + room_x + room_x * 0.8f * static_cast<float>(Base::Math::sine(minute * 0.7));
    float const y = area.y + room_y + text_height + room_y * 0.8f * static_cast<float>(Base::Math::sine(minute * 1.1));
    graphics.draw_text(text.view(), font, { x, y }, Color::from_rgb(0xffffff, 200));
}

String ScreenSaver::accessibility_label() const
{
    return MUST(String::from_view(m_is_clock ? "Clock"_sv : "Drift"_sv));
}

}
