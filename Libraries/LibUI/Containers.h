/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibUI/Controls.h>
#include <LibUI/Layout.h>
#include <LibUI/TextField.h>

// What holds other views: split panes, tabs, a toolbar, a sidebar.

namespace UI {

enum class DividerStyle : u8 {
    Thin,
    Thick,
    PaneSplitter,
};

class SplitView : public View {
public:
    explicit SplitView(Axis axis = Axis::Horizontal, DividerStyle = DividerStyle::Thin);

    // Panes in order. Its size along the axis, and the least it may shrink to. The last
    // pane takes what is left.
    template<typename T, typename... Args>
    T& add_pane(float size, float minimum, Args&&... arguments)
    {
        auto& pane = add<T>(forward<Args>(arguments)...);
        m_panes.append(Pane { &pane, size, minimum });
        set_needs_layout();
        return pane;
    }
    void set_pane_size(size_t index, float size);
    [[nodiscard]] float pane_size(size_t index) const { return m_panes[index].size; }

    Function<void()> on_resize_pane;

    void layout() override;
    void draw_over_children(Graphics&) override;
    [[nodiscard]] View* hit_test(Gfx::FloatPoint in_parent) override;
    bool mouse_down(MouseEvent const&) override;
    void mouse_dragged(MouseEvent const&) override;
    void mouse_up(MouseEvent const&) override;
    void mouse_moved(MouseEvent const&) override;
    [[nodiscard]] WS::CursorShape cursor() const override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::SplitGroup; }

private:
    struct Pane {
        View* view;
        float size;
        float minimum;
    };

    [[nodiscard]] float thickness() const;
    [[nodiscard]] int divider_at(Gfx::FloatPoint) const;

    Vector<Pane> m_panes;
    Axis m_axis;
    DividerStyle m_divider;
    int m_dragging { -1 };
    float m_drag_from { 0 };
    float m_drag_start_size { 0 };
    bool m_over_divider { false };
};

class TabView : public View {
public:
    TabView();

    template<typename T, typename... Args>
    T& add_tab(StringView title, Args&&... arguments)
    {
        auto& page = m_pages_parent->add<T>(forward<Args>(arguments)...);
        register_tab(title, page);
        return page;
    }
    void select_tab(size_t);
    [[nodiscard]] size_t selected_tab() const { return m_selected; }

    Function<void(size_t)> on_change;

    void layout() override;
    void draw(Graphics&) override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::TabGroup; }

private:
    void register_tab(StringView title, View&);

    SegmentedControl* m_strip { nullptr };
    View* m_pages_parent { nullptr };
    Vector<View*> m_pages;
    size_t m_selected { 0 };
};

class ToolbarButton;

// Across the top of a window with a unified title: it draws the bar the title would be
// in, moves the window by its empty parts, and keeps clear of the traffic lights.
class Toolbar : public View {
public:
    Toolbar();

    enum class Style : u8 {
        Unified,
        Compact,
    };
    void set_style(Style);
    // The window's title, next to the lights.
    void set_title(StringView);
    // Clear of the lights by default; less where a sidebar has them.
    void set_left_padding(float);

    ToolbarButton& add_button(Symbol, Function<void()> action, StringView label = {});
    SearchField& add_search(float width);
    SegmentedControl& add_segmented();
    template<typename T, typename... Args>
    T& add_item(Args&&... arguments)
    {
        return m_items->add<T>(forward<Args>(arguments)...);
    }
    // Everything after this goes to the right.
    void add_flexible_space();

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void layout() override;
    void draw(Graphics&) override;
    bool mouse_down(MouseEvent const&) override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Toolbar; }

private:
    View* m_items { nullptr };
    String m_title;
    Style m_style { Style::Unified };
    float m_left_padding { -1 };
};

class ToolbarButton final : public Control {
public:
    explicit ToolbarButton(Symbol, StringView label = {});

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    void mouse_entered() override;
    void mouse_exited() override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Button; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_label.view())); }
    bool accessibility_press() override;

private:
    Symbol m_symbol;
    String m_label;
};

// The list down the side of a window, over a material: sections and rows.
class Sidebar : public View {
public:
    Sidebar();

    // Room at the top, for a window's traffic lights when it reaches up to them.
    void set_top_padding(float);
    void add_section(StringView title);
    size_t add_row(StringView title, Symbol = Symbol::Folder);
    // In the same section as that row, right after it. Rows after it move along one.
    size_t insert_row_after(size_t row, StringView title, Symbol = Symbol::Folder);
    void remove_row(size_t);
    // Everything, sections too.
    void clear();
    // A small button at the end of a row, as a volume has to eject it.
    void set_row_accessory(size_t row, Symbol, StringView label, Function<void()> action);
    void select_row(size_t);
    [[nodiscard]] int selected_row() const { return m_selected; }
    [[nodiscard]] StringView row_title(size_t row) const;

    Function<void(size_t)> on_select;

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    bool mouse_down(MouseEvent const&) override;
    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    [[nodiscard]] bool shows_focus_ring() const override { return false; }
    bool key_down(KeyEvent const&) override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Table; }
    void accessibility_parts(Vector<Accessibility::Node>&) override;

private:
    struct Entry {
        String title;
        Symbol symbol;
        bool section;
        int row;
        bool has_accessory { false };
        Symbol accessory { Symbol::Eject };
        String accessory_label {};
        Function<void()> accessory_action {};
    };

    [[nodiscard]] float y_of(size_t entry) const;

    Vector<Entry> m_entries;
    size_t m_rows { 0 };
    int m_selected { -1 };
    float m_top_padding { -1 };
};

}
