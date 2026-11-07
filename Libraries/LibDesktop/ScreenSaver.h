/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/String.h>
#include <LibUI/Graphics.h>
#include <LibUI/View.h>

// What covers the screen when nobody's been using the computer for a while, or the pointer's put in a corner that
// starts it: our own, as Apple's aren't ours. "Drift": soft lights moving slowly over the dark. "Clock": the time, large,
// in a different place each minute so that nothing stays in one place. The login window runs it; Settings shows it small.
namespace Desktop {

class ScreenSaver final : public UI::View {
public:
    // "Drift" or "Clock"; anything else is Drift.
    explicit ScreenSaver(StringView name);

    // Their names, in the order Settings shows them.
    [[nodiscard]] static Span<StringView const> names();
    // Moves on to this many seconds since it began.
    void advance(double seconds);
    // The time the Clock shows: by default, now.
    void set_time(i64 seconds_since_1970, i32 offset_seconds);

    void draw(UI::Graphics&) override;
    // Drawn into any rectangle, as Settings' previews are.
    void draw_in(UI::Graphics&, Gfx::FloatRect);
    [[nodiscard]] UI::Accessibility::Role accessibility_role() const override { return UI::Accessibility::Role::Group; }
    [[nodiscard]] String accessibility_label() const override;

private:
    bool m_is_clock { false };
    double m_seconds { 0 };
    bool m_has_time { false };
    i64 m_time { 0 };
    i32 m_offset { 0 };
};

}
