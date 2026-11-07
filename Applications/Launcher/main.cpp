/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibUI/Application.h>
#include <LibUI/Motion.h>
#include <LibUI/TextField.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

// Every application, in a grid over the whole screen, in pages, as Launchpad has them: in the order they've been put in,
// some in folders. A click or Return opens one and it goes away, as it does on Escape and on a click anywhere else.
// Dragged, one moves the others along to make room; held over another, they become a folder, named by what kind of
// applications they are. The order and the folders are kept in com.vulture.launcher.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

String copy(StringView text)
{
    return MUST(String::from_view(text));
}

bool contains_ignoring_case(StringView text, StringView part)
{
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    for (size_t i = 0; i + part.length() <= text.length(); ++i) {
        bool same = true;
        for (size_t j = 0; j < part.length() && same; ++j)
            same = lower(text[i + j]) == lower(part[j]);
        if (same)
            return true;
    }
    return false;
}

// A folder's name, from what kind of applications are in it, as Launchpad names them by their App Store category.
StringView folder_name_for(StringView category)
{
    struct Name {
        StringView category;
        StringView name;
    };
    static constexpr Name names[] {
        { "public.app-category.utilities"_sv, "Utilities"_sv },
        { "public.app-category.productivity"_sv, "Productivity"_sv },
        { "public.app-category.developer-tools"_sv, "Developer Tools"_sv },
        { "public.app-category.graphics-design"_sv, "Graphics & Design"_sv },
        { "public.app-category.entertainment"_sv, "Entertainment"_sv },
        { "public.app-category.music"_sv, "Music"_sv },
        { "public.app-category.photography"_sv, "Photography"_sv },
        { "public.app-category.reference"_sv, "Reference"_sv },
    };
    for (auto const& one : names) {
        if (one.category == category)
            return one.name;
    }
    return "Untitled"_sv;
}

// What's in the grid: an application, or a folder of them.
struct Item {
    // Into the applications: for an application, its own; for a folder, what's in it.
    Vector<size_t> applications;
    bool folder { false };
    String name;
};

class Grid;

// A folder, open over the grid: its name above, which can be changed, and its applications in a grid of their own.
class FolderNameField final : public TextField {
public:
    FolderNameField()
        : TextField({}, FieldStyle::Plain)
    {
        set_accessibility_label("Folder Name"_sv);
    }

    Function<void()> on_commit;

    bool key_down(KeyEvent const& event) override
    {
        if (event.code == KeyEnter || event.code == KeyEscape) {
            if (on_commit)
                on_commit();
            return true;
        }
        return TextField::key_down(event);
    }
};

class Grid final : public View {
public:
    Grid()
    {
        m_all = Desktop::installed_applications();
        load();
        m_name_field = &add<FolderNameField>();
        m_name_field->set_hidden(true);
        m_name_field->on_commit = [this] { commit_name(); };
        filter({});
    }

    Function<void()> on_finished;
    Function<void()> on_wants_keyboard;

    void filter(StringView text)
    {
        m_searching = !text.is_empty();
        m_found.clear();
        if (m_searching) {
            for (size_t i = 0; i < m_all.size(); ++i) {
                if (is_launcher(i))
                    continue;
                if (contains_ignoring_case(m_all[i].name.view(), text) || contains_ignoring_case(m_all[i].kind.view(), text))
                    m_found.append(i);
            }
        }
        m_page = 0;
        m_selected = shown_count() == 0 ? -1 : 0;
        set_needs_display();
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        // Behind an open folder, nothing of the grid: as Launchpad's, too blurred to see.
        if (m_open_folder >= 0) {
            draw_folder(graphics);
            return;
        }
        size_t const per_page = rows() * columns();
        size_t const first = static_cast<size_t>(m_page) * per_page;
        auto const order = shown_order();
        for (size_t slot = first; slot < order.size() && slot < first + per_page; ++slot) {
            if (order[slot] < 0)
                continue;
            auto const rect = cell_rect(slot - first);
            bool const merging = m_drag.has_value() && m_merge_target == order[slot];
            bool const selected = static_cast<int>(slot) == m_selected && !m_drag.has_value() && (m_searching || m_keyboard_used);
            draw_entry(graphics, static_cast<size_t>(order[slot]), rect, selected, merging);
        }
        // A dot for each page.
        int const pages = page_count();
        if (pages > 1) {
            float const spacing = t.metric("launcher.dotSpacing"_sv);
            float x = (width() - spacing * static_cast<float>(pages - 1)) / 2;
            for (int i = 0; i < pages; ++i)
                graphics.fill_circle({ x + static_cast<float>(i) * spacing, height() - t.metric("launcher.dotBottom"_sv) }, t.metric("launcher.dotSize"_sv) / 2, t.color("launcher.text"_sv).with_opacity(i == m_page ? 1 : 0.35f));
        }
        // What's being dragged, under the pointer, over everything.
        if (m_drag.has_value()) {
            float const icon = t.metric("launcher.iconSize"_sv);
            FloatRect const at { m_drag_point.x - icon / 2, m_drag_point.y - icon / 2, icon, icon };
            draw_icon(graphics, m_drag->item, at, graphics.scale(), 1);
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        m_press_point = event.position;
        m_pressed = -1;
        m_pressed_in_folder = -1;
        if (m_open_folder >= 0) {
            if (folder_title_rect().contains(event.position)) {
                start_renaming();
                return true;
            }
            auto const index = folder_cell_at(event.position);
            if (index >= 0) {
                m_pressed_in_folder = index;
                return true;
            }
            if (!folder_panel_rect().contains(event.position)) {
                close_folder();
                return true;
            }
            return true;
        }
        int const slot = slot_at(event.position);
        if (slot >= 0) {
            m_pressed = slot;
            return true;
        }
        if (on_finished)
            on_finished();
        return true;
    }

    void mouse_dragged(MouseEvent const& event) override
    {
        if (m_searching)
            return;
        float const dx = event.position.x - m_press_point.x;
        float const dy = event.position.y - m_press_point.y;
        if (!m_drag.has_value() && sqrtf(dx * dx + dy * dy) < tokens().metric("launcher.dragStart"_sv))
            return;
        if (!m_drag.has_value())
            begin_drag();
        if (!m_drag.has_value())
            return;
        m_drag_point = event.position;
        update_drag();
        set_needs_display();
    }

    void mouse_up(MouseEvent const& event) override
    {
        if (m_drag.has_value()) {
            finish_drag();
            return;
        }
        if (m_pressed_in_folder >= 0 && folder_cell_at(event.position) == m_pressed_in_folder) {
            auto const& folder = m_items[static_cast<size_t>(m_open_folder)];
            open_application(folder.applications[static_cast<size_t>(m_pressed_in_folder)]);
            return;
        }
        if (m_pressed >= 0 && slot_at(event.position) == m_pressed)
            activate_slot(m_pressed);
    }

    bool scroll_wheel(ScrollEvent const& event) override
    {
        if (m_open_folder >= 0)
            return true;
        if (event.delta_y > 0 && m_page + 1 < page_count())
            ++m_page;
        else if (event.delta_y < 0 && m_page > 0)
            --m_page;
        set_needs_display();
        return true;
    }

    bool handle_key(KeyEvent const& event)
    {
        if (event.code == KeyEscape) {
            if (m_renaming) {
                commit_name();
                return true;
            }
            if (m_open_folder >= 0) {
                close_folder();
                return true;
            }
            return false;
        }
        int const across = static_cast<int>(columns());
        int const count = static_cast<int>(shown_count());
        if (count == 0 || m_open_folder >= 0)
            return false;
        int next = m_selected;
        switch (event.code) {
        case KeyLeft:
            next -= 1;
            break;
        case KeyRight:
            next += 1;
            break;
        case KeyUp:
            next -= across;
            break;
        case KeyDown:
            next += across;
            break;
        case KeyEnter:
            if (m_selected >= 0)
                activate_slot(m_selected);
            return true;
        default:
            return false;
        }
        m_selected = clamp(next, 0, count - 1);
        m_keyboard_used = true;
        m_page = m_selected / static_cast<int>(rows() * columns());
        set_needs_display();
        return true;
    }

    void layout() override
    {
        if (m_renaming)
            m_name_field->set_frame(folder_title_rect());
    }

    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        if (m_open_folder >= 0) {
            auto const& folder = m_items[static_cast<size_t>(m_open_folder)];
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Group, folder.name.view(), folder_panel_rect(), "open folder"_sv));
            if (!m_renaming)
                parts.append(Accessibility::part_of(*this, Accessibility::Role::StaticText, folder.name.view(), folder_title_rect()));
            for (size_t i = 0; i < folder.applications.size(); ++i)
                parts.append(Accessibility::part_of(*this, Accessibility::Role::Cell, m_all[folder.applications[i]].name.view(), folder_cell_rect(i)));
            return;
        }
        size_t const per_page = rows() * columns();
        size_t const first = static_cast<size_t>(m_page) * per_page;
        auto const order = shown_order();
        for (size_t slot = first; slot < order.size() && slot < first + per_page; ++slot) {
            if (order[slot] < 0)
                continue;
            StringView value = static_cast<int>(slot) == m_selected ? "selected"_sv : StringView {};
            if (!m_searching && m_items[static_cast<size_t>(order[slot])].folder)
                value = "folder"_sv;
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Cell, entry_name(static_cast<size_t>(order[slot])), cell_rect(slot - first), value));
        }
    }

private:
    struct Drag {
        Item item;
        // Where it came from: a slot of the grid, or of the open folder's.
        int from { -1 };
        bool from_folder { false };
    };

    // ---- What's shown: the items in their order, or what was searched for.

    [[nodiscard]] bool is_launcher(size_t application) const { return m_all[application].identifier.view() == "com.vulture.launcher"_sv; }

    [[nodiscard]] size_t shown_count() const { return m_searching ? m_found.size() : m_items.size(); }

    [[nodiscard]] StringView entry_name(size_t entry) const
    {
        if (m_searching)
            return m_all[m_found[entry]].name.view();
        auto const& item = m_items[entry];
        return item.folder ? item.name.view() : m_all[item.applications[0]].name.view();
    }

    // Each slot's entry, with a gap (-1) where what's dragged would go.
    [[nodiscard]] Vector<int> shown_order() const
    {
        Vector<int> order;
        for (size_t i = 0; i < shown_count(); ++i)
            order.append(static_cast<int>(i));
        if (m_drag.has_value()) {
            int const at = clamp(m_drop_slot, 0, static_cast<int>(order.size()));
            order.insert(static_cast<size_t>(at), -1);
        }
        return order;
    }

    void load()
    {
        m_items.clear();
        Vector<bool> placed;
        for (size_t i = 0; i < m_all.size(); ++i)
            placed.append(false);
        auto find = [&](StringView identifier) -> int {
            for (size_t i = 0; i < m_all.size(); ++i) {
                if (m_all[i].identifier.view() == identifier && !placed[i] && !is_launcher(i))
                    return static_cast<int>(i);
            }
            return -1;
        };
        Desktop::Preferences preferences("com.vulture.launcher"_sv);
        if (auto const* layout = preferences.array("layout"_sv)) {
            for (size_t i = 0; i < layout->size(); ++i) {
                auto const& entry = layout->at(i);
                if (entry.is_string()) {
                    int const found = find(entry.as_string());
                    if (found < 0)
                        continue;
                    placed[static_cast<size_t>(found)] = true;
                    Item item;
                    item.applications.append(static_cast<size_t>(found));
                    m_items.append(move(item));
                    continue;
                }
                auto const* members = entry.get_array("items"_sv);
                if (!members)
                    continue;
                Item folder;
                folder.folder = true;
                folder.name = copy(entry.get_string("folder"_sv).value_or("Untitled"_sv));
                for (size_t m = 0; m < members->size(); ++m) {
                    if (!members->at(m).is_string())
                        continue;
                    int const found = find(members->at(m).as_string());
                    if (found < 0)
                        continue;
                    placed[static_cast<size_t>(found)] = true;
                    folder.applications.append(static_cast<size_t>(found));
                }
                if (!folder.applications.is_empty())
                    m_items.append(move(folder));
            }
        }
        // New ones at the end.
        for (size_t i = 0; i < m_all.size(); ++i) {
            if (placed[i] || is_launcher(i))
                continue;
            Item item;
            item.applications.append(i);
            m_items.append(move(item));
        }
    }

    void save()
    {
        auto layout = JSON::Value::array();
        for (auto const& item : m_items) {
            if (!item.folder) {
                MUST(layout.append(MUST(JSON::Value::string(m_all[item.applications[0]].identifier.view()))));
                continue;
            }
            auto folder = JSON::Value::object();
            MUST(folder.set("folder"_sv, MUST(JSON::Value::string(item.name.view()))));
            auto members = JSON::Value::array();
            for (auto application : item.applications)
                MUST(members.append(MUST(JSON::Value::string(m_all[application].identifier.view()))));
            MUST(folder.set("items"_sv, move(members)));
            MUST(layout.append(move(folder)));
        }
        Desktop::Preferences preferences("com.vulture.launcher"_sv);
        preferences.set("layout"_sv, move(layout));
        preferences.save();
    }

    // ---- Where things are.

    [[nodiscard]] size_t columns() const { return static_cast<size_t>(tokens().metric("launcher.columns"_sv)); }
    [[nodiscard]] size_t rows() const { return max(static_cast<size_t>((height() - tokens().metric("launcher.bottomReserve"_sv)) / tokens().metric("launcher.cellHeight"_sv)), static_cast<size_t>(1)); }
    [[nodiscard]] int page_count() const
    {
        size_t const per_page = rows() * columns();
        size_t const count = shown_count() + (m_drag.has_value() ? 1 : 0);
        return max(static_cast<int>((count + per_page - 1) / per_page), 1);
    }

    [[nodiscard]] FloatRect cell_rect(size_t index) const
    {
        auto const& t = tokens();
        float const cell_width = t.metric("launcher.cellWidth"_sv);
        float const cell_height = t.metric("launcher.cellHeight"_sv);
        size_t const across = columns();
        float const left = (width() - static_cast<float>(across) * cell_width) / 2;
        return { left + static_cast<float>(index % across) * cell_width, static_cast<float>(index / across) * cell_height, cell_width, cell_height };
    }

    [[nodiscard]] FloatRect icon_rect(FloatRect cell) const
    {
        float const icon = tokens().metric("launcher.iconSize"_sv);
        return { cell.x + (cell.width - icon) / 2, cell.y, icon, icon };
    }

    [[nodiscard]] int slot_at(FloatPoint point) const
    {
        size_t const per_page = rows() * columns();
        size_t const first = static_cast<size_t>(m_page) * per_page;
        for (size_t slot = first; slot < shown_count() && slot < first + per_page; ++slot) {
            if (cell_rect(slot - first).contains(point))
                return static_cast<int>(slot);
        }
        return -1;
    }

    // ---- Drawing.

    void draw_icon(Graphics& graphics, Item const& item, FloatRect at, float scale, float opacity) const
    {
        auto const& t = tokens();
        if (!item.folder) {
            if (auto icon = Desktop::icon_named(m_all[item.applications[0]].icon.view(), static_cast<int>(at.width), static_cast<int>(scale)))
                graphics.painter().draw_bitmap(*icon, at);
            (void)opacity;
            return;
        }
        // A folder: a plate with the first nine of its applications on it, small.
        float const inset = at.width * t.metric("launcher.folderInset"_sv);
        FloatRect const plate { at.x + inset, at.y + inset, at.width - inset * 2, at.height - inset * 2 };
        graphics.fill_continuous(plate, plate.width * t.metric("launcher.folderRadius"_sv), t.color("launcher.folder"_sv));
        float const gap = plate.width * 0.08f;
        float const mini = (plate.width - gap * 4) / 3;
        for (size_t i = 0; i < item.applications.size() && i < 9; ++i) {
            FloatRect const place { plate.x + gap + (mini + gap) * static_cast<float>(i % 3), plate.y + gap + (mini + gap) * static_cast<float>(i / 3), mini, mini };
            if (auto icon = Desktop::icon_named(m_all[item.applications[i]].icon.view(), static_cast<int>(mini), static_cast<int>(scale)))
                graphics.painter().draw_bitmap(*icon, place);
        }
    }

    void draw_entry(Graphics& graphics, size_t entry, FloatRect rect, bool selected, bool merging) const
    {
        auto const& t = tokens();
        auto const icon = icon_rect(rect);
        if (selected)
            graphics.fill_rounded({ icon.x - 8, icon.y - 8, icon.width + 16, icon.height + 16 }, 18, t.color("launcher.selection"_sv));
        if (merging)
            graphics.fill_continuous({ icon.x - 6, icon.y - 6, icon.width + 12, icon.height + 12 }, (icon.width + 12) * t.metric("launcher.folderRadius"_sv), t.color("launcher.mergeTarget"_sv));
        if (m_searching) {
            Item item;
            item.applications.append(m_found[entry]);
            draw_icon(graphics, item, icon, graphics.scale(), 1);
        } else {
            draw_icon(graphics, m_items[entry], icon, graphics.scale(), 1);
        }
        auto const font = t.named_font("launcherName"_sv);
        auto const name = entry_name(entry);
        float const text_width = min(Appearance::text_width(font, name), rect.width - 8);
        graphics.draw_text_truncated(name, font, { rect.x + (rect.width - text_width) / 2, icon.y + icon.height + t.metric("launcher.nameGap"_sv) }, rect.width - 8, t.color("launcher.text"_sv));
    }

    // ---- An open folder.

    [[nodiscard]] FloatRect folder_panel_rect() const
    {
        auto const& t = tokens();
        float const cell_width = t.metric("launcher.cellWidth"_sv);
        float const cell_height = t.metric("launcher.cellHeight"_sv);
        float const padding = t.metric("launcher.folderPadding"_sv);
        size_t const count = m_open_folder >= 0 ? m_items[static_cast<size_t>(m_open_folder)].applications.size() : 0;
        size_t const across = static_cast<size_t>(t.metric("launcher.folderColumns"_sv));
        size_t const down = max(static_cast<size_t>(1), min((count + across - 1) / across, static_cast<size_t>(t.metric("launcher.folderRows"_sv))));
        float const panel_width = static_cast<float>(across) * cell_width + padding * 2;
        float const panel_height = static_cast<float>(down) * cell_height + padding * 2;
        return { (width() - panel_width) / 2, (height() - panel_height) / 2, panel_width, panel_height };
    }

    [[nodiscard]] FloatRect folder_title_rect() const
    {
        auto const& t = tokens();
        auto const panel = folder_panel_rect();
        float const height = t.metric("launcher.folderTitleHeight"_sv);
        float const width = t.metric("launcher.folderTitleWidth"_sv);
        return { panel.x + (panel.width - width) / 2, panel.y - height - t.metric("launcher.folderTitleGap"_sv), width, height };
    }

    [[nodiscard]] FloatRect folder_cell_rect(size_t index) const
    {
        auto const& t = tokens();
        auto const panel = folder_panel_rect();
        float const padding = t.metric("launcher.folderPadding"_sv);
        size_t const across = static_cast<size_t>(t.metric("launcher.folderColumns"_sv));
        float const cell_width = t.metric("launcher.cellWidth"_sv);
        float const cell_height = t.metric("launcher.cellHeight"_sv);
        return { panel.x + padding + static_cast<float>(index % across) * cell_width, panel.y + padding + static_cast<float>(index / across) * cell_height, cell_width, cell_height };
    }

    [[nodiscard]] int folder_cell_at(FloatPoint point) const
    {
        if (m_open_folder < 0)
            return -1;
        auto const& folder = m_items[static_cast<size_t>(m_open_folder)];
        for (size_t i = 0; i < folder.applications.size(); ++i) {
            if (folder_cell_rect(i).contains(point))
                return static_cast<int>(i);
        }
        return -1;
    }

    void draw_folder(Graphics& graphics) const
    {
        auto const& t = tokens();
        graphics.fill({ 0, 0, width(), height() }, t.color("launcher.dim"_sv));
        auto const panel = folder_panel_rect();
        graphics.fill_continuous(panel, t.metric("launcher.folderPanelRadius"_sv), t.color("launcher.folderPanel"_sv));
        auto const& folder = m_items[static_cast<size_t>(m_open_folder)];
        if (!m_renaming)
            graphics.draw_text_centered(folder.name.view(), t.named_font("launcherFolderTitle"_sv), folder_title_rect(), t.color("launcher.text"_sv));
        auto const font = t.named_font("launcherName"_sv);
        for (size_t i = 0; i < folder.applications.size(); ++i) {
            if (m_drag.has_value() && m_drag->from_folder && static_cast<size_t>(m_drag->from) == i)
                continue;
            auto const rect = folder_cell_rect(i);
            auto const icon = icon_rect(rect);
            Item one;
            one.applications.append(folder.applications[i]);
            draw_icon(graphics, one, icon, graphics.scale(), 1);
            auto const name = m_all[folder.applications[i]].name.view();
            float const text_width = min(Appearance::text_width(font, name), rect.width - 8);
            graphics.draw_text_truncated(name, font, { rect.x + (rect.width - text_width) / 2, icon.y + icon.height + t.metric("launcher.nameGap"_sv) }, rect.width - 8, t.color("launcher.text"_sv));
        }
    }

    void open_folder(size_t item)
    {
        m_open_folder = static_cast<int>(item);
        printf("launcher: folder %s\n", m_items[item].name.characters());
        fflush(stdout);
        set_needs_display();
        if (parent())
            parent()->set_needs_display();
    }

    void close_folder()
    {
        if (m_renaming)
            commit_name();
        if (m_open_folder < 0)
            return;
        // A folder left with one application is that application again, as in Launchpad.
        auto& folder = m_items[static_cast<size_t>(m_open_folder)];
        if (folder.applications.size() == 1) {
            folder.folder = false;
            folder.name = {};
            save();
        } else if (folder.applications.is_empty()) {
            m_items.remove(static_cast<size_t>(m_open_folder));
            save();
        }
        m_open_folder = -1;
        set_needs_display();
        if (parent())
            parent()->set_needs_display();
    }

    void start_renaming()
    {
        m_renaming = true;
        m_name_field->set_text(m_items[static_cast<size_t>(m_open_folder)].name.view());
        m_name_field->set_hidden(false);
        m_name_field->set_frame(folder_title_rect());
        if (on_wants_keyboard)
            on_wants_keyboard();
        set_needs_display();
    }

    void commit_name()
    {
        if (!m_renaming)
            return;
        m_renaming = false;
        auto const typed = m_name_field->text();
        if (!typed.is_empty() && m_open_folder >= 0) {
            m_items[static_cast<size_t>(m_open_folder)].name = copy(typed);
            save();
            printf("launcher: renamed %.*s\n", static_cast<int>(typed.length()), typed.characters_without_null_termination());
            fflush(stdout);
        }
        m_name_field->set_hidden(true);
        if (on_name_done)
            on_name_done();
        set_needs_display();
    }

public:
    // The search field has the keyboard again.
    Function<void()> on_name_done;
    [[nodiscard]] bool has_open_folder() const { return m_open_folder >= 0; }
    [[nodiscard]] TextField& name_field() { return *m_name_field; }

private:
    // ---- Dragging: to another place, onto another (a folder), or out of a folder.

    void begin_drag()
    {
        if (m_open_folder >= 0) {
            if (m_pressed_in_folder < 0)
                return;
            auto& folder = m_items[static_cast<size_t>(m_open_folder)];
            Item one;
            one.applications.append(folder.applications[static_cast<size_t>(m_pressed_in_folder)]);
            folder.applications.remove(static_cast<size_t>(m_pressed_in_folder));
            m_drag = Drag { move(one), m_pressed_in_folder, true };
            // Out of the folder, onto the grid, as soon as it leaves it.
            close_folder();
        } else {
            if (m_pressed < 0)
                return;
            auto item = move(m_items[static_cast<size_t>(m_pressed)]);
            m_items.remove(static_cast<size_t>(m_pressed));
            m_drag = Drag { move(item), m_pressed, false };
        }
        m_drop_slot = m_drag->from_folder ? static_cast<int>(m_items.size()) : m_drag->from;
        m_merge_target = -1;
        printf("launcher: dragging\n");
        fflush(stdout);
    }

    void update_drag()
    {
        auto const& t = tokens();
        size_t const per_page = rows() * columns();
        size_t const first = static_cast<size_t>(m_page) * per_page;
        // Over another one's middle: held there a moment, they'd make a folder.
        int over = -1;
        for (size_t entry = first; entry < m_items.size() && entry < first + per_page; ++entry) {
            // Where it is with the gap as it is now.
            size_t const slot = static_cast<int>(entry) >= m_drop_slot ? entry + 1 : entry;
            if (slot < first || slot >= first + per_page)
                continue;
            auto const icon = icon_rect(cell_rect(slot - first));
            float const reach = icon.width * t.metric("launcher.mergeReach"_sv);
            FloatPoint const middle { icon.x + icon.width / 2, icon.y + icon.height / 2 };
            float const dx = m_drag_point.x - middle.x;
            float const dy = m_drag_point.y - middle.y;
            if (sqrtf(dx * dx + dy * dy) <= reach && !m_drag->item.folder)
                over = static_cast<int>(entry);
        }
        if (over != m_hover_target) {
            m_hover_target = over;
            if (m_merge_timer >= 0) {
                Application::the().cancel_timer(m_merge_timer);
                m_merge_timer = -1;
            }
            if (over < 0) {
                m_merge_target = -1;
            } else {
                m_merge_timer = Application::the().add_timer(static_cast<u64>(t.metric("launcher.mergeMilliseconds"_sv)), false, [this] {
                    m_merge_timer = -1;
                    m_merge_target = m_hover_target;
                    set_needs_display();
                });
            }
        }
        if (over >= 0)
            return;
        // Otherwise, the others move along to make room where it is: not when the pointer is over the gap itself.
        for (size_t index = 0; index < per_page; ++index) {
            if (cell_rect(index).contains(m_drag_point)) {
                m_drop_slot = min(static_cast<int>(first + index), static_cast<int>(m_items.size()));
                break;
            }
        }
        m_merge_target = -1;
        // At the left or right edge, a moment, the page turns.
        float const edge = t.metric("launcher.pageEdge"_sv);
        int const direction = m_drag_point.x < edge ? -1 : (m_drag_point.x > width() - edge ? 1 : 0);
        if (direction != m_turn_direction) {
            m_turn_direction = direction;
            if (m_turn_timer >= 0) {
                Application::the().cancel_timer(m_turn_timer);
                m_turn_timer = -1;
            }
            if (direction != 0) {
                m_turn_timer = Application::the().add_timer(static_cast<u64>(t.metric("launcher.turnMilliseconds"_sv)), false, [this] {
                    m_turn_timer = -1;
                    int const next = m_page + m_turn_direction;
                    if (next >= 0 && next < page_count())
                        m_page = next;
                    m_turn_direction = 0;
                    set_needs_display();
                });
            }
        }
    }

    void finish_drag()
    {
        int* const timers[] { &m_merge_timer, &m_turn_timer };
        for (int* timer : timers) {
            if (*timer >= 0) {
                Application::the().cancel_timer(*timer);
                *timer = -1;
            }
        }
        auto drag = m_drag.release_value();
        if (m_merge_target >= 0 && static_cast<size_t>(m_merge_target) < m_items.size()) {
            auto& target = m_items[static_cast<size_t>(m_merge_target)];
            if (!target.folder) {
                // Two applications: a folder, named by what kind the one held over is.
                target.folder = true;
                target.name = copy(folder_name_for(m_all[target.applications[0]].category.view()));
            }
            for (auto application : drag.item.applications)
                target.applications.append(application);
            printf("launcher: into folder %s\n", target.name.characters());
            fflush(stdout);
        } else {
            size_t const at = static_cast<size_t>(clamp(m_drop_slot, 0, static_cast<int>(m_items.size())));
            m_items.insert(at, move(drag.item));
            printf("launcher: moved to %zu\n", at);
            fflush(stdout);
        }
        m_merge_target = -1;
        m_hover_target = -1;
        m_drop_slot = -1;
        save();
        set_needs_display();
    }

    // ---- Opening.

    void activate_slot(int slot)
    {
        if (m_searching) {
            open_application(m_found[static_cast<size_t>(slot)]);
            return;
        }
        auto const& item = m_items[static_cast<size_t>(slot)];
        if (item.folder) {
            open_folder(static_cast<size_t>(slot));
            return;
        }
        open_application(item.applications[0]);
    }

    void open_application(size_t application)
    {
        (void)Desktop::launch(m_all[application]);
        if (on_finished)
            on_finished();
    }

    Vector<Desktop::AppInfo> m_all;
    Vector<Item> m_items;
    Vector<size_t> m_found;
    bool m_searching { false };
    int m_page { 0 };
    int m_selected { -1 };
    int m_open_folder { -1 };
    bool m_keyboard_used { false };
    bool m_renaming { false };
    FolderNameField* m_name_field { nullptr };
    FloatPoint m_press_point;
    int m_pressed { -1 };
    int m_pressed_in_folder { -1 };
    Optional<Drag> m_drag;
    FloatPoint m_drag_point;
    int m_drop_slot { -1 };
    int m_hover_target { -1 };
    int m_merge_target { -1 };
    int m_merge_timer { -1 };
    int m_turn_direction { 0 };
    int m_turn_timer { -1 };
};

class SearchBox final : public TextField {
public:
    SearchBox()
        : TextField({}, FieldStyle::Rounded)
    {
        set_placeholder("Search"_sv);
    }

    Function<bool(KeyEvent const&)> on_key;

    bool key_down(KeyEvent const& event) override
    {
        if (on_key && on_key(event))
            return true;
        return TextField::key_down(event);
    }
};

class Screen final : public View {
public:
    Screen()
    {
        m_search = &add<SearchBox>();
        m_grid = &add<Grid>();
        m_search->on_change = [this] { m_grid->filter(m_search->text()); };
        m_search->on_key = [this](KeyEvent const& event) {
            if (event.code == KeyEscape) {
                if (m_grid->handle_key(event))
                    return true;
                if (m_grid->on_finished)
                    m_grid->on_finished();
                return true;
            }
            return m_grid->handle_key(event);
        };
        m_grid->on_wants_keyboard = [this] {
            if (window())
                window()->make_first_responder(&m_grid->name_field());
        };
        m_grid->on_name_done = [this] {
            if (window())
                window()->make_first_responder(m_search);
        };
    }

    [[nodiscard]] SearchBox& search() { return *m_search; }
    [[nodiscard]] Grid& grid() { return *m_grid; }

    void layout() override
    {
        auto const& t = tokens();
        float const search_width = t.metric("launcher.searchWidth"_sv);
        float const top = t.metric("launcher.topPadding"_sv);
        m_search->set_frame({ (width() - search_width) / 2, top, search_width, m_search->intrinsic_size().height });
        float const grid_top = top + t.metric("launcher.gridTop"_sv);
        m_grid->set_frame({ 0, grid_top, width(), height() - grid_top });
    }

    void draw(Graphics& graphics) override
    {
        graphics.fill({ 0, 0, width(), height() }, tokens().color("launcher.fill"_sv));
    }

    void draw_over_children(Graphics& graphics) override
    {
        // An open folder dims all else, the search field too.
        if (m_grid->has_open_folder())
            graphics.fill({ 0, 0, width(), m_grid->frame().y }, tokens().color("launcher.dim"_sv));
    }

private:
    SearchBox* m_search { nullptr };
    Grid* m_grid { nullptr };
};

}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    bool still = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--still") == 0)
            still = true;
    }
    auto application_or_error = Application::create();
    if (application_or_error.is_error())
        return 1;
    auto application = application_or_error.release_value();
    if (still)
        set_motion_enabled(false);
    auto screen = Platform::the().screen_size();
    auto window = adopt_own(*new UI::Window("Launcher"_sv, { 0, 0, screen.width, screen.height }, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
    window->set_transparent(true);
    window->backend().set_level(WS::LevelPopup);
    MaterialRegion material { { 0, 0, screen.width, screen.height }, WS::Material::UnderWindow };
    window->set_materials(Span<MaterialRegion const>(&material, 1));
    auto& view = window->set_content<Screen>();
    auto* app = application.ptr();
    view.grid().on_finished = [app] { app->quit(); };
    window->on_key_change = [app](bool active) {
        if (!active)
            app->quit();
    };
    window->show();
    window->update_layout_now();
    window->make_first_responder(&view.search());
    printf("launcher: shown\n");
    fflush(stdout);
    return application->exec();
}
