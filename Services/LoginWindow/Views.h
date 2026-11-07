/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/String.h>
#include <LibUI/Controls.h>
#include <LibUI/Graphics.h>
#include <LibUI/View.h>

// What the login window, the lock screen and Setup are made of.
namespace LoginWindow {

// The screenshots' own moment when still (Thursday, January 15 2026, 9:41), as the shell has it.
void set_still(bool);
[[nodiscard]] bool is_still();
struct Moment {
    i64 seconds { 0 };
    i32 offset { 0 };
};
[[nodiscard]] Moment now();


// The date, and the time large under it, at the top of the screen, as Sequoia's login and lock screens have them.
class Clock final : public UI::View {
public:
    Clock();
    // Shown again when the minute changes.
    void tick();

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(UI::Graphics&) override;
    [[nodiscard]] UI::Accessibility::Role accessibility_role() const override { return UI::Accessibility::Role::StaticText; }
    [[nodiscard]] String accessibility_label() const override;

private:
    String m_date;
    String m_time;
};

// Restart and Shut Down, round, with what they do under them.
class PowerButton final : public UI::Control {
public:
    PowerButton(StringView title, bool is_restart, Function<void()> action);

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(UI::Graphics&) override;
    [[nodiscard]] UI::Accessibility::Role accessibility_role() const override { return UI::Accessibility::Role::Button; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_title.view())); }

private:
    String m_title;
    bool m_is_restart;
};

// Someone to choose: their picture, their name under it, and a tick under that if they're logged in already.
class UserTile final : public UI::Control {
public:
    UserTile(StringView name, StringView full_name, bool logged_in, Function<void()> action);

    [[nodiscard]] StringView name() const { return m_name.view(); }
    [[nodiscard]] StringView full_name() const { return m_full_name.view(); }
    void set_logged_in(bool);

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(UI::Graphics&) override;
    [[nodiscard]] UI::Accessibility::Role accessibility_role() const override { return UI::Accessibility::Role::Button; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_full_name.view())); }
    [[nodiscard]] String accessibility_value() const override;

private:
    String m_name;
    String m_full_name;
    bool m_logged_in { false };
};

// A picture alone: the chosen person's, over the password field.
class Picture final : public UI::View {
public:
    explicit Picture(float size);
    void set_full_name(StringView);

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override { return { m_size, m_size }; }
    void draw(UI::Graphics&) override;

private:
    String m_full_name;
    float m_size;
};

}
