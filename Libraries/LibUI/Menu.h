/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibUI/Controls.h>
#include <LibUI/TextField.h>
#include <LibUI/Window.h>

namespace UI {

class Menu;

struct MenuItem {
    MenuItem() = default;
    MenuItem(MenuItem&&) = default;

    String title;
    Function<void()> action;
    // Command, and this key: 0 for none. The text is what shows, "S" or "⇧S".
    u16 shortcut_code { 0 };
    u32 shortcut_modifiers { WS::ModifierCommand };
    String shortcut_text;
    OwnPtr<Menu> submenu;
    bool separator { false };
    bool enabled { true };
    bool checked { false };
};

// A list of items, kept in place: an item's address is what its menu's published form
// refers to. Those added later go on the end, or at a place.
class MenuItems {
public:
    MenuItem& append(MenuItem item)
    {
        auto owned = adopt_own(*new MenuItem(move(item)));
        auto& result = *owned;
        m_items.append(move(owned));
        return result;
    }
    // A title's item here, for its submenu to be set.
    MenuItem& insert_returning(size_t index, StringView title)
    {
        auto owned = adopt_own(*new MenuItem);
        owned->title = MUST(String::from_view(title));
        auto& result = *owned;
        m_items.insert(index, move(owned));
        return result;
    }
    [[nodiscard]] MenuItem& operator[](size_t index) { return *m_items[index]; }
    [[nodiscard]] MenuItem const& operator[](size_t index) const { return *m_items[index]; }
    [[nodiscard]] size_t size() const { return m_items.size(); }
    [[nodiscard]] bool is_empty() const { return m_items.is_empty(); }
    void clear() { m_items.clear(); }

    class Iterator {
    public:
        explicit Iterator(NonnullOwnPtr<MenuItem>* at)
            : m_at(at)
        {
        }
        MenuItem& operator*() const { return **m_at; }
        Iterator& operator++()
        {
            ++m_at;
            return *this;
        }
        bool operator!=(Iterator const& other) const { return m_at != other.m_at; }

    private:
        NonnullOwnPtr<MenuItem>* m_at;
    };
    [[nodiscard]] Iterator begin() { return Iterator(m_items.data()); }
    [[nodiscard]] Iterator end() { return Iterator(m_items.data() + m_items.size()); }

private:
    Vector<NonnullOwnPtr<MenuItem>> m_items;
};

class Menu {
public:
    explicit Menu(StringView title = {});
    ~Menu();

    [[nodiscard]] StringView title() const { return m_title.view(); }
    MenuItem& add(StringView title, Function<void()> action = nullptr);
    MenuItem& add_with_shortcut(StringView title, u16 code, StringView shown, Function<void()> action, u32 modifiers = WS::ModifierCommand);
    void add_separator();
    Menu& add_submenu(StringView title);
    [[nodiscard]] MenuItems& items() { return m_items; }

    // Opens it from `owner`, at this point on the screen; with `aligned` naming an item,
    // that item sits at the point, as a pop-up button's does. It closes by itself.
    void open(Window& owner, Gfx::IntPoint screen_point, int aligned = -1);
    void close();
    [[nodiscard]] bool is_open() const { return m_window.ptr() != nullptr; }
    [[nodiscard]] Window* window() const { return m_window.ptr(); }
    // Whatever the choice was, or none: after the action.
    Function<void()> on_close;
    // The pointer is outside every menu of the chain: a menu bar, to slide along.
    Function<void(Gfx::FloatPoint screen)> on_pointer_outside;

    // A key's item anywhere in it, for a window's shortcuts.
    bool perform_shortcut(KeyEvent const&);

    // What it would measure.
    [[nodiscard]] Gfx::FloatSize size() const;

    // The one open at the root of a chain, if any.
    [[nodiscard]] static Menu* open_menu();

    // What the view inside it calls.
    void pointer_at(Gfx::FloatPoint screen, u32 buttons);
    void released_at(Gfx::FloatPoint screen);
    void choose(int index);
    void set_highlight(int index);
    [[nodiscard]] int highlight() const { return m_highlight; }
    bool key(KeyEvent const&);

private:
    friend class MenuView;

    [[nodiscard]] int item_at(Gfx::FloatPoint in_window) const;
    [[nodiscard]] Gfx::FloatRect item_rect(int index) const;
    void open_submenu(int index);
    void close_submenu();
    [[nodiscard]] Menu* deepest();
    [[nodiscard]] bool screen_point_in_chain(Gfx::FloatPoint) const;

    String m_title;
    MenuItems m_items;
    OwnPtr<Window> m_window;
    class KeyForwarder;
    OwnPtr<KeyForwarder> m_key_forwarder;
    Window* m_owner { nullptr };
    Menu* m_parent { nullptr };
    Menu* m_open_child { nullptr };
    int m_highlight { -1 };
    bool m_chosen { false };
    // Let go soon after opening stays open; let go after dragging onto an item chooses.
    double m_opened_at { 0 };
    bool m_dragged { false };
};

// A choice of items, shown as a menu when pressed.
class PopUpButton : public Control {
public:
    explicit PopUpButton(bool pulls_down = false);
    ~PopUpButton() override;

    void add_item(StringView title);
    void add_separator();
    void set_selected_index(int);
    [[nodiscard]] int selected_index() const { return m_selected; }
    [[nodiscard]] StringView selected_title() const;
    // For a pull-down, the title stays this, and items are commands.
    void set_title(StringView);
    [[nodiscard]] size_t item_count() const { return m_menu->items().size(); }

    Function<void(int)> on_change;

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    [[nodiscard]] float baseline() const override;
    void draw(Graphics&) override;
    [[nodiscard]] float focus_ring_radius() const override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::PopUpButton; }
    [[nodiscard]] String accessibility_value() const override;
    bool accessibility_press() override;

    void open_menu();
    [[nodiscard]] Menu& menu() { return *m_menu; }

protected:
    bool mouse_down(MouseEvent const&) override;
    bool key_down(KeyEvent const&) override;

private:
    OwnPtr<Menu> m_menu;
    Vector<int> m_item_map;
    int m_selected { -1 };
    bool m_pulls_down;
    String m_title;
};

// A field whose text can come from a list.
class ComboBox final : public TextField {
public:
    ComboBox();

    void add_item(StringView);

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    [[nodiscard]] float baseline() const override;
    [[nodiscard]] Insets overflow() const override { return { 2.5f, 2.5f, 3, 2.5f }; }
    [[nodiscard]] float focus_ring_radius() const override;

protected:
    bool mouse_down(MouseEvent const&) override;
    [[nodiscard]] float text_left() const override;
    [[nodiscard]] float text_right_margin() const override;
    [[nodiscard]] float text_baseline() const override;
    void draw_face(Graphics&) override;
    void draw_decorations(Graphics&) override;
    [[nodiscard]] StringView text_color_token() const override { return "label"_sv; }

private:
    [[nodiscard]] Gfx::FloatRect arrow_rect() const;

    OwnPtr<Menu> m_menu;
};

}
