/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Common.h"
#include <LibBase/HashMap.h>
#include <LibGfx/Bitmap.h>
#include <LibUI/Containers.h>
#include <LibUI/Controls.h>
#include <LibUI/Window.h>

// What a folder can be shown as, besides icons and a list; and what's said about what's selected.

namespace FilesApp {

// A few at a time, as each is a process of its own.
class Thumbnails {
public:
    static Thumbnails& the();
    // What's known of it: nothing (yet, or ever) is a null.
    [[nodiscard]] Gfx::Bitmap const* get(FileItem const&, int pixels);
    Function<void()> on_new;

private:
    void pump();

    struct Waiting {
        String key;
        String path;
        int pixels;
    };
    HashMap<String, RefPtr<Gfx::Bitmap>> m_done;
    HashMap<String, bool> m_asked;
    Vector<Waiting> m_waiting;
    int m_running { 0 };
};

// Seven circles, on or off.
class TagPicker final : public UI::View {
public:
    void set_path(StringView);
    Function<void()> on_change;

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(UI::Graphics&) override;
    bool mouse_down(UI::MouseEvent const&) override;
    [[nodiscard]] UI::Accessibility::Role accessibility_role() const override { return UI::Accessibility::Role::Group; }
    void accessibility_parts(Vector<UI::Accessibility::Node>&) override;

private:
    [[nodiscard]] Gfx::FloatRect dot_rect(size_t) const;
    String m_path;
    Vector<String> m_tags;
};

// Where this folder is: each one above it, to go back up to.
class PathBar final : public UI::View {
public:
    void set_path(StringView);
    Function<void(StringView)> on_navigate;

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(UI::Graphics&) override;
    bool mouse_down(UI::MouseEvent const&) override;
    [[nodiscard]] UI::Accessibility::Role accessibility_role() const override { return UI::Accessibility::Role::Group; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view("Path"_sv)); }
    void accessibility_parts(Vector<UI::Accessibility::Node>&) override;

private:
    struct Crumb {
        String name;
        String path;
        Gfx::FloatRect rect;
    };
    Vector<Crumb> m_crumbs;
};

// What's selected, looked at more closely: beside any view, and in Gallery.
class PreviewPane final : public UI::View {
public:
    void set_path(StringView);
    Function<void()> on_changed;
    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(UI::Graphics&) override;
    void layout() override;
    [[nodiscard]] UI::Accessibility::Role accessibility_role() const override { return UI::Accessibility::Role::Group; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view("Preview"_sv)); }
    [[nodiscard]] String accessibility_value() const override { return MUST(String::from_view(m_item.has_value() ? m_item->name.view() : StringView {})); }

private:
    void update_accessories();

    Optional<FileItem> m_item;
    RefPtr<Gfx::Bitmap> m_picture;
    u32 m_generation { 0 };
    TagPicker* m_tags { nullptr };
    UI::Button* m_rotate_left { nullptr };
    UI::Button* m_rotate_right { nullptr };
};

class ColumnBrowser final : public UI::View {
public:
    ColumnBrowser();
    void set_root(StringView);
    // The rightmost chosen, as what menus act on.
    [[nodiscard]] Vector<String> selected_paths() const;
    void refresh();
    Function<void()> on_selection_change;
    Function<void(StringView)> on_open;
    Function<void(UI::MouseEvent const&, StringView path)> on_context_menu;
    Function<void()> on_quick_view;

    void draw(UI::Graphics&) override;
    bool mouse_down(UI::MouseEvent const&) override;
    bool scroll_wheel(UI::ScrollEvent const&) override;
    bool key_down(UI::KeyEvent const&) override;
    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    [[nodiscard]] bool shows_focus_ring() const override { return false; }
    [[nodiscard]] UI::Accessibility::Role accessibility_role() const override { return UI::Accessibility::Role::Group; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view("Columns"_sv)); }
    void accessibility_parts(Vector<UI::Accessibility::Node>&) override;

private:
    struct Column {
        String path;
        Vector<FileItem> items;
        int selected { -1 };
        float scroll { 0 };
    };
    void select(size_t column, int row);
    void bring_into_view(size_t column);
    [[nodiscard]] float column_x(size_t) const;
    [[nodiscard]] int rightmost_with_selection() const;
    Vector<Column> m_columns;
    float m_scroll { 0 };
    PreviewPane* m_preview { nullptr };
};

class GalleryView final : public UI::View {
public:
    GalleryView();
    void set_items(Vector<FileItem>* items);
    void select_index(size_t);
    void refresh_selection();
    Function<void()> on_selection_change;
    Function<void(StringView)> on_open;
    Function<void()> on_quick_view;

    void draw(UI::Graphics&) override;
    void layout() override;
    bool mouse_down(UI::MouseEvent const&) override;
    bool key_down(UI::KeyEvent const&) override;
    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    [[nodiscard]] bool shows_focus_ring() const override { return false; }
    [[nodiscard]] UI::Accessibility::Role accessibility_role() const override { return UI::Accessibility::Role::Group; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view("Gallery"_sv)); }
    void accessibility_parts(Vector<UI::Accessibility::Node>&) override;

private:
    [[nodiscard]] int selected() const;
    [[nodiscard]] Gfx::FloatRect thumbnail_rect(size_t) const;
    void load_picture();
    Vector<FileItem>* m_items { nullptr };
    RefPtr<Gfx::Bitmap> m_picture;
    u32 m_generation { 0 };
    float m_strip_scroll { 0 };
    PreviewPane* m_info { nullptr };
};

// Cmd-I.
// `on_change`: when its tags are changed, for whatever shows them to show them.
void show_get_info(StringView path, Function<void()> on_change = {});

}
