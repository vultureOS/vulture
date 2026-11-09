/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibUI/Controls.h>
#include <LibUI/TextStorage.h>

namespace UI {

enum class FieldStyle : u8 {
    Square,
    Rounded,
    // No face at all: for text edited in place.
    Plain,
};

// One line of text to edit.
class TextField : public Control {
public:
    explicit TextField(StringView text = {}, FieldStyle = FieldStyle::Square);
    ~TextField() override;

    [[nodiscard]] StringView text() const { return m_storage.text(); }
    void set_text(StringView);
    void set_placeholder(StringView);
    [[nodiscard]] StringView placeholder() const { return m_placeholder.view(); }
    void set_style(FieldStyle);
    // Shows dots, and copies nothing.
    void set_secure(bool);
    void set_editable(bool);
    [[nodiscard]] TextStorage& storage() { return m_storage; }
    void select_all();
    [[nodiscard]] StringView selected_text_for_services() const override { return m_secure ? StringView {} : m_storage.selected_text(); }

    // After each edit; on Return or leaving, with what it holds.
    Function<void()> on_change;
    Function<void()> on_commit;

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    [[nodiscard]] float baseline() const override;
    void draw(Graphics&) override;
    [[nodiscard]] Insets overflow() const override { return { 0, 0, 0, 0 }; }
    [[nodiscard]] WS::CursorShape cursor() const override { return WS::CursorShape::IBeam; }
    [[nodiscard]] float focus_ring_radius() const override;

    [[nodiscard]] bool accepts_first_responder() const override { return m_editable; }
    [[nodiscard]] bool can_take_key_focus() const override { return m_editable && is_enabled() && is_visible(); }
    void did_become_first_responder() override;
    void did_resign_first_responder() override;
    bool key_down(KeyEvent const&) override;
    bool perform_command(StringView) override;
    [[nodiscard]] bool can_perform_command(StringView) const override;

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::TextField; }
    [[nodiscard]] String accessibility_label() const override;
    [[nodiscard]] String accessibility_value() const override;

protected:
    bool mouse_down(MouseEvent const&) override;
    void mouse_dragged(MouseEvent const&) override;
    void mouse_up(MouseEvent const&) override;

    // Where the text may go: a search field keeps room for its icons.
    [[nodiscard]] virtual float text_left() const;
    [[nodiscard]] virtual float text_right_margin() const;
    [[nodiscard]] virtual float text_baseline() const;
    virtual void draw_face(Graphics&);
    virtual void draw_decorations(Graphics&) { }
    [[nodiscard]] virtual StringView text_color_token() const { return "fieldText"_sv; }

private:
    [[nodiscard]] StringView shown_text() const;
    [[nodiscard]] float x_of_offset(size_t) const;
    [[nodiscard]] size_t offset_at_x(float) const;
    void scroll_caret_into_view();
    void restart_blink();
    void stop_blink();

    TextStorage m_storage;
    String m_placeholder;
    FieldStyle m_style;
    bool m_secure { false };
    bool m_editable { true };
    float m_scroll { 0 };
    bool m_caret_shown { false };
    int m_blink_timer { -1 };
    bool m_selecting { false };
    size_t m_select_from { 0 };
    int m_select_unit { 1 };
    size_t m_word_start { 0 };
    size_t m_word_end { 0 };
    bool m_pending_click_selection { false };
    size_t m_pending_click_offset { 0 };
    mutable String m_dots;
};

class SearchField final : public TextField {
public:
    SearchField();

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    [[nodiscard]] float baseline() const override;
    [[nodiscard]] float focus_ring_radius() const override;

protected:
    bool mouse_down(MouseEvent const&) override;
    [[nodiscard]] float text_left() const override;
    [[nodiscard]] float text_right_margin() const override;
    [[nodiscard]] float text_baseline() const override;
    void draw_face(Graphics&) override;
    void draw_decorations(Graphics&) override;

private:
    [[nodiscard]] Gfx::FloatRect clear_rect() const;
};

}
