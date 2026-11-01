/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibUI/TextStorage.h>
#include <LibUI/View.h>

namespace UI {

// Lines of text to edit, wrapped to the width (with no wrapping, for code). Lives in
// a ScrollView, which it keeps its caret in sight of.
class TextView : public View {
public:
    TextView();
    ~TextView() override;

    [[nodiscard]] StringView text() const { return m_storage.text(); }
    void set_text(StringView);
    [[nodiscard]] TextStorage& storage() { return m_storage; }
    void set_font(FontSpec);
    void set_wraps(bool);
    void set_editable(bool);
    [[nodiscard]] bool is_editable() const { return m_editable; }
    void set_text_inset(float);

    Function<void()> on_change;

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void layout() override;
    void draw(Graphics&) override;
    [[nodiscard]] WS::CursorShape cursor() const override { return WS::CursorShape::IBeam; }

    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    [[nodiscard]] bool shows_focus_ring() const override { return false; }
    void did_become_first_responder() override;
    void did_resign_first_responder() override;
    bool key_down(KeyEvent const&) override;
    bool perform_command(StringView) override;
    [[nodiscard]] bool can_perform_command(StringView) const override;

    bool mouse_down(MouseEvent const&) override;
    void mouse_dragged(MouseEvent const&) override;
    void mouse_up(MouseEvent const&) override;

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::TextArea; }
    [[nodiscard]] String accessibility_value() const override;

    // Where this offset's caret goes, in the view.
    [[nodiscard]] Gfx::FloatRect caret_rect(size_t offset) const;
    [[nodiscard]] size_t offset_at(Gfx::FloatPoint) const;
    [[nodiscard]] size_t visual_line_count() const { return m_lines.size(); }

private:
    struct Line {
        size_t start;
        size_t end;
    };

    void relayout_lines();
    [[nodiscard]] size_t line_of(size_t offset) const;
    void scroll_caret_into_view();
    void restart_blink();
    void stop_blink();
    void moved(bool extend);
    [[nodiscard]] float x_in_line(Line const&, size_t offset) const;
    [[nodiscard]] size_t offset_in_line(Line const&, float x) const;

    TextStorage m_storage;
    FontSpec m_font;
    Vector<Line> m_lines;
    float m_laid_out_width { -1 };
    float m_inset { 4 };
    bool m_wraps { true };
    bool m_editable { true };
    bool m_caret_shown { false };
    int m_blink_timer { -1 };
    // The column up and down keep going back to.
    float m_goal_x { -1 };
    bool m_selecting { false };
    size_t m_select_from { 0 };
    int m_select_unit { 1 };
    size_t m_unit_start { 0 };
    size_t m_unit_end { 0 };
};

}
