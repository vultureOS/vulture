/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibBase/Math.h>
#include <LibUI/Accessibility.h>
#include <LibUI/Application.h>
#include <LibUI/Menu.h>
#include <LibUI/Motion.h>
#include <dirent.h>
#include <notify.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The Dock: what is kept there, what was used lately and what is running, on a shelf of frosted glass that swells under
// the pointer, along the bottom of the screen or down one side, there all the time or only when the pointer comes to its
// edge. Folders kept there open out as stacks; what has a window minimized keeps a tile for it; what is dropped on the
// Trash goes there. What it keeps is kept as a Mac keeps its own (com.apple.dock's names, in com.vulture.dock).

using namespace UI;
using namespace Gfx;
namespace Math = Base::Math;

namespace {

bool s_still;

Appearance const& tokens()
{
    return Appearance::the();
}

WindowServerProxy& server()
{
    return Application::the().connection().server();
}

String copy(StringView text)
{
    return MUST(String::from_view(text));
}

constexpr StringView preferences_name = "com.vulture.dock"_sv;
// Posted by whoever changes them (Settings, `defaults`), and by the Dock itself.
constexpr char const* preferences_changed = "com.vulture.dock.preferences";

enum class Edge : u8 {
    Bottom,
    Left,
    Right,
};

// What is kept there to begin with: Files always first.
StringView const s_default_apps[] = {
    "/System/Library/CoreServices/Files.app"_sv,
    "/System/Applications/Launcher.app"_sv,
    "/System/Applications/Settings.app"_sv,
    "/System/Applications/Terminal.app"_sv,
    "/System/Applications/Text Editor.app"_sv,
    "/System/Applications/Viewer.app"_sv,
    "/System/Applications/Calculator.app"_sv,
    "/System/Applications/Monitor.app"_sv,
    "/System/Applications/Widget Gallery.app"_sv,
};

constexpr size_t most_recents = 3;

// How a folder kept in the Dock looks, and how it opens.
enum class StackLook : u8 {
    Stack,
    Folder,
};
enum class StackView : u8 {
    Automatic,
    Fan,
    Grid,
    List,
};

struct Settings {
    Vector<String> apps;
    Vector<String> others;
    Vector<String> recents;
    Edge edge { Edge::Bottom };
    bool autohide { false };
    bool magnification { true };
    float tile_size { 0 };
    float large_size { 0 };
    bool show_recents { true };
    bool indicators { true };
    bool launch_animation { true };
    StackLook stack_look { StackLook::Stack };
    StackView stack_view { StackView::Automatic };

    static Settings load()
    {
        Settings settings;
        Desktop::Preferences preferences(preferences_name);
        auto strings = [&](StringView key, Vector<String>& into) {
            auto const* list = preferences.array(key);
            if (!list)
                return false;
            for (size_t i = 0; i < list->size(); ++i) {
                if (list->at(i).is_string())
                    into.append(copy(list->at(i).as_string()));
            }
            return true;
        };
        if (!strings("persistent-apps"_sv, settings.apps)) {
            for (auto path : s_default_apps)
                settings.apps.append(copy(path));
        }
        if (!strings("persistent-others"_sv, settings.others)) {
            StringBuilder downloads;
            downloads.append(Desktop::home_directory().view());
            downloads.append("/Downloads"_sv);
            settings.others.append(MUST(downloads.to_string()));
        }
        (void)strings("recent-apps"_sv, settings.recents);
        auto const orientation = preferences.string("orientation"_sv).value_or("bottom"_sv);
        settings.edge = orientation == "left"_sv ? Edge::Left : (orientation == "right"_sv ? Edge::Right : Edge::Bottom);
        settings.autohide = preferences.boolean("autohide"_sv).value_or(false);
        settings.magnification = preferences.boolean("magnification"_sv).value_or(true);
        settings.tile_size = clamp(static_cast<float>(preferences.number("tilesize"_sv).value_or(static_cast<double>(tokens().metric("dock.iconSize"_sv)))), 16.0f, 128.0f);
        settings.large_size = clamp(static_cast<float>(preferences.number("largesize"_sv).value_or(static_cast<double>(tokens().metric("dock.magnifiedSize"_sv)))), 16.0f, 128.0f);
        settings.show_recents = preferences.boolean("show-recents"_sv).value_or(true);
        settings.indicators = preferences.boolean("show-process-indicators"_sv).value_or(true);
        settings.launch_animation = preferences.boolean("launchanim"_sv).value_or(true);
        settings.stack_look = preferences.string("stack-display"_sv).value_or("stack"_sv) == "folder"_sv ? StackLook::Folder : StackLook::Stack;
        auto const view = preferences.string("stack-view"_sv).value_or("automatic"_sv);
        settings.stack_view = view == "fan"_sv ? StackView::Fan : (view == "grid"_sv ? StackView::Grid : (view == "list"_sv ? StackView::List : StackView::Automatic));
        return settings;
    }

    void save() const
    {
        Desktop::Preferences preferences(preferences_name);
        auto strings = [](Vector<String> const& from) {
            auto list = JSON::Value::array();
            for (auto const& one : from)
                MUST(list.append(MUST(JSON::Value::string(one.view()))));
            return list;
        };
        preferences.set("persistent-apps"_sv, strings(apps));
        preferences.set("persistent-others"_sv, strings(others));
        preferences.set("recent-apps"_sv, strings(recents));
        preferences.set("orientation"_sv, MUST(JSON::Value::string(edge == Edge::Left ? "left"_sv : (edge == Edge::Right ? "right"_sv : "bottom"_sv))));
        preferences.set("autohide"_sv, JSON::Value(autohide));
        preferences.set("stack-display"_sv, MUST(JSON::Value::string(stack_look == StackLook::Folder ? "folder"_sv : "stack"_sv)));
        preferences.set("stack-view"_sv, MUST(JSON::Value::string(stack_view == StackView::Fan ? "fan"_sv : (stack_view == StackView::Grid ? "grid"_sv : (stack_view == StackView::List ? "list"_sv : "automatic"_sv)))));
        preferences.save();
    }
};

struct Tile {
    enum class Kind : u8 {
        Application,
        Separator,
        Folder,
        MinimizedWindow,
        Trash,
    };
    Kind kind { Kind::Application };
    String title;
    String icon;
    String identifier;
    // The bundle, or the folder.
    String path;
    i32 pid { 0 };
    u32 window_id { 0 };
    bool pinned { false };
    bool recent { false };
    String badge;
    // Where it comes out now: its middle along the Dock, and how big.
    float center { 0 };
    float size { 0 };
    // Bouncing since (seconds), while it starts.
    double bouncing_since { -1 };
};

// What's in a folder, newest first, as a stack shows it.
struct StackItem {
    String name;
    String path;
    String icon;
    i64 modified { 0 };
};

Vector<StackItem> items_of(StringView folder)
{
    Vector<StackItem> items;
    auto terminated = copy(folder);
    DIR* directory = opendir(terminated.characters());
    if (!directory)
        return items;
    while (auto* entry = readdir(directory)) {
        StringView const name { entry->d_name, strlen(entry->d_name) };
        if (name.starts_with('.'))
            continue;
        StringBuilder path;
        path.append(folder);
        path.append('/');
        path.append(name);
        StackItem item;
        item.name = copy(name);
        item.path = MUST(path.to_string());
        item.icon = copy(Desktop::icon_name_for_path(item.path.view()).view());
        struct stat information {};
        if (stat(item.path.characters(), &information) == 0)
            item.modified = static_cast<i64>(information.st_mtime);
        items.append(move(item));
    }
    closedir(directory);
    for (size_t i = 1; i < items.size(); ++i) {
        for (size_t j = i; j > 0 && (items[j].modified > items[j - 1].modified || (items[j].modified == items[j - 1].modified && items[j].name.view() < items[j - 1].name.view())); --j) {
            auto moving = move(items[j]);
            items[j] = move(items[j - 1]);
            items[j - 1] = move(moving);
        }
    }
    return items;
}

StringView last_component(StringView path)
{
    size_t at = path.length();
    while (at > 0 && path[at - 1] != '/')
        --at;
    return path.substring_view(at, path.length() - at);
}

class DockView;

// A folder kept in the Dock, opened: its items fanned out above it, or in a grid, with a way into Files.
class StackPopup final : public View {
public:
    enum class Shape : u8 {
        Fan,
        Grid,
    };

    StackPopup(Shape shape, String folder, Vector<StackItem> items, Function<void()> on_done)
        : m_shape(shape)
        , m_folder(move(folder))
        , m_items(move(items))
        , m_on_done(move(on_done))
    {
    }

    [[nodiscard]] static FloatSize size_for(Shape shape, size_t count)
    {
        auto const& t = tokens();
        if (shape == Shape::Fan) {
            size_t const shown = min(count, static_cast<size_t>(t.metric("dock.fanMost"_sv)));
            return { t.metric("dock.fanWidth"_sv), t.metric("dock.fanRow"_sv) * static_cast<float>(shown + 1) + t.metric("dock.fanPadding"_sv) * 2 };
        }
        int const columns = clamp(static_cast<int>(count), 3, static_cast<int>(t.metric("dock.gridColumns"_sv)));
        int const rows = max(1, (static_cast<int>(count) + columns - 1) / columns);
        float const cell = t.metric("dock.gridCell"_sv);
        float const padding = t.metric("dock.gridPadding"_sv);
        return { cell * static_cast<float>(columns) + padding * 2, t.metric("dock.gridTitle"_sv) + cell * static_cast<float>(rows) + t.metric("dock.gridFooter"_sv) + padding };
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        int const scale = static_cast<int>(graphics.scale());
        if (m_shape == Shape::Fan) {
            // From the bottom up: the newest nearest the Dock, then "Open in Files" at the top.
            float const row = t.metric("dock.fanRow"_sv);
            float const icon = t.metric("dock.fanIcon"_sv);
            float const padding = t.metric("dock.fanPadding"_sv);
            auto const font = t.named_font("body"_sv);
            size_t const shown = shown_count();
            for (size_t i = 0; i <= shown; ++i) {
                float const y = height() - padding - row * static_cast<float>(i + 1);
                // The fan curves a little away from the Dock as it goes up.
                float const lean = static_cast<float>(i * i) * t.metric("dock.fanLean"_sv);
                FloatRect const picture { width() - padding - icon - lean, y + (row - icon) / 2, icon, icon };
                StringView const name = i < shown ? m_items[i].name.view() : "Open in Files"_sv;
                if (i < shown) {
                    if (auto bitmap = Desktop::icon_named(m_items[i].icon.view(), static_cast<int>(icon), scale))
                        graphics.painter().draw_bitmap(*bitmap, picture);
                } else {
                    graphics.fill_circle({ picture.x + icon / 2, picture.y + icon / 2 }, icon * 0.36f, t.color("dock.fanArrowFill"_sv));
                    graphics.draw_symbol(Symbol::ChevronRight, picture.inflated(-icon * 0.28f), t.color("dock.fanArrow"_sv), 2);
                }
                float const text = Appearance::text_width(font, name);
                float const label_width = min(text + 20, picture.x - 12);
                FloatRect const label { picture.x - 8 - label_width, y + (row - t.metric("dock.labelHeight"_sv)) / 2, label_width, t.metric("dock.labelHeight"_sv) };
                graphics.fill_rounded(label, t.metric("dock.labelHeight"_sv) / 2, i == static_cast<size_t>(m_hovered) ? t.color("dock.fanLabelHovered"_sv) : t.color("dock.labelBackground"_sv));
                graphics.draw_text_centered(name, font, label, t.color("dock.labelText"_sv));
            }
            return;
        }
        // A grid in a panel: the folder's name, its items, and a way into Files.
        float const padding = t.metric("dock.gridPadding"_sv);
        float const cell = t.metric("dock.gridCell"_sv);
        float const icon = t.metric("dock.gridIcon"_sv);
        float const title = t.metric("dock.gridTitle"_sv);
        graphics.draw_text_centered(last_component(m_folder.view()), t.named_font("headline"_sv), { 0, 0, width(), title }, t.color("label"_sv));
        int const columns = columns_of();
        auto const small = t.named_font("small"_sv);
        for (size_t i = 0; i < m_items.size(); ++i) {
            int const column = static_cast<int>(i) % columns;
            int const row = static_cast<int>(i) / columns;
            FloatRect const place { padding + cell * static_cast<float>(column), title + cell * static_cast<float>(row), cell, cell };
            if (static_cast<int>(i) == m_hovered)
                graphics.fill_rounded(place.inflated(-3), 6, t.color("dock.gridHover"_sv));
            if (auto bitmap = Desktop::icon_named(m_items[i].icon.view(), static_cast<int>(icon), scale))
                graphics.painter().draw_bitmap(*bitmap, FloatRect { place.x + (cell - icon) / 2, place.y + 6, icon, icon });
            graphics.draw_text_truncated(m_items[i].name.view(), small, { place.x + 4, place.y + 6 + icon + 14 }, cell - 8, t.color("label"_sv));
        }
        auto const footer = footer_rect();
        graphics.draw_text_centered("Open in Files"_sv, t.named_font("body"_sv), footer, m_hovered == footer_index() ? t.color("accent"_sv) : t.color("secondaryLabel"_sv));
    }

    void mouse_moved(MouseEvent const& event) override
    {
        int const over = index_at(event.position);
        if (over != m_hovered) {
            m_hovered = over;
            set_needs_display();
        }
    }

    bool mouse_down(MouseEvent const&) override { return true; }

    void mouse_up(MouseEvent const& event) override
    {
        int const index = index_at(event.position);
        // Let go anywhere else: put away.
        if (index < 0) {
            if (!FloatRect { 0, 0, width(), height() }.contains(event.position) && m_on_done)
                m_on_done();
            return;
        }
        if (index == footer_index())
            (void)Desktop::open_path(m_folder.view());
        else
            (void)Desktop::open_path(m_items[static_cast<size_t>(index)].path.view());
        if (m_on_done)
            m_on_done();
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    [[nodiscard]] String accessibility_label() const override { return copy(last_component(m_folder.view())); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        for (int i = 0; i < footer_index(); ++i)
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, m_items[static_cast<size_t>(i)].name.view(), rect_of(i)));
        parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, "Open in Files"_sv, rect_of(footer_index())));
    }

private:
    [[nodiscard]] size_t shown_count() const { return min(m_items.size(), static_cast<size_t>(tokens().metric("dock.fanMost"_sv))); }
    [[nodiscard]] int footer_index() const { return static_cast<int>(m_shape == Shape::Fan ? shown_count() : m_items.size()); }
    [[nodiscard]] int columns_of() const { return clamp(static_cast<int>(m_items.size()), 3, static_cast<int>(tokens().metric("dock.gridColumns"_sv))); }

    [[nodiscard]] FloatRect footer_rect() const
    {
        auto const& t = tokens();
        return { 0, height() - t.metric("dock.gridFooter"_sv) - t.metric("dock.gridPadding"_sv) / 2, width(), t.metric("dock.gridFooter"_sv) };
    }

    [[nodiscard]] FloatRect rect_of(int index) const
    {
        auto const& t = tokens();
        if (m_shape == Shape::Fan) {
            float const row = t.metric("dock.fanRow"_sv);
            float const y = height() - t.metric("dock.fanPadding"_sv) - row * static_cast<float>(index + 1);
            return { 0, y, width(), row };
        }
        if (index == footer_index())
            return footer_rect();
        float const cell = t.metric("dock.gridCell"_sv);
        int const columns = columns_of();
        return { t.metric("dock.gridPadding"_sv) + cell * static_cast<float>(index % columns), t.metric("dock.gridTitle"_sv) + cell * static_cast<float>(index / columns), cell, cell };
    }

    [[nodiscard]] int index_at(FloatPoint point) const
    {
        for (int i = 0; i <= footer_index(); ++i) {
            if (rect_of(i).contains(point))
                return i;
        }
        return -1;
    }

    Shape m_shape;
    String m_folder;
    Vector<StackItem> m_items;
    Function<void()> m_on_done;
    int m_hovered { -1 };
};

class DockView final : public View {
public:
    DockView()
    {
        m_settings = Settings::load();
        rebuild();
    }

    [[nodiscard]] Settings const& settings() const { return m_settings; }

    // Which applications' icons may have badges, as Settings > Notifications says ("Badge application icon").
    void reload_badge_settings()
    {
        m_no_badges.clear();
        Desktop::Preferences preferences("com.vulture.notifications"_sv);
        auto const* all = preferences.object("applications"_sv);
        if (!all)
            return;
        for (size_t i = 0; i < all->size(); ++i) {
            auto const& one = all->at(i);
            if (one.is_object() && !one.get_boolean("badge"_sv).value_or(true))
                m_no_badges.append(copy(all->key_at(i)));
        }
    }

    [[nodiscard]] bool badges_allowed(StringView application) const
    {
        for (auto const& name : m_no_badges) {
            if (name.view() == application)
                return false;
        }
        return true;
    }

    // Changed by someone else: taken up again, and the Dock put where it's now meant to be.
    void reload_settings()
    {
        auto const edge = m_settings.edge;
        auto const autohide = m_settings.autohide;
        m_settings = Settings::load();
        if (on_placement_change && (edge != m_settings.edge || autohide != m_settings.autohide))
            on_placement_change();
        m_hide = m_settings.autohide && !m_hovered_inside ? 1 : 0;
        m_hide_target = m_hide;
        if (m_hide_animation) {
            motion().cancel(m_hide_animation);
            m_hide_animation = 0;
        }
        rebuild();
    }

    Function<void()> on_placement_change;

    void toggle_autohide()
    {
        m_settings.autohide = !m_settings.autohide;
        m_settings.save();
        notify_post(preferences_changed);
    }

    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        compute_layout();
        for (auto const& tile : m_tiles) {
            if (tile.kind == Tile::Kind::Separator)
                continue;
            auto node = Accessibility::part_of(*this, Accessibility::Role::Button, tile.title.view(), icon_rect(tile, 0), tile.badge.view());
            parts.append(move(node));
        }
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Group; }
    [[nodiscard]] String accessibility_label() const override { return copy(m_settings.autohide && m_hide >= 1 ? "Dock, hidden"_sv : "Dock"_sv); }

    void set_state(Desktop::ShellState const& state)
    {
        // What stopped running, and isn't kept there, is one of the recent ones now.
        Vector<String> ended;
        for (auto const& before : m_state_applications) {
            bool still = false;
            for (auto const& application : state.applications)
                still = still || application.pid == before.pid;
            if (!still && !before.bundle_path.is_empty())
                ended.append(copy(before.bundle_path.view()));
        }
        m_state_applications.clear();
        for (auto const& application : state.applications) {
            if (application.identifier.view() == "com.vulture.Files"_sv)
                m_files_has_windows = !application.windows.is_empty();
            RunningApplication running;
            running.pid = application.pid;
            running.identifier = copy(application.identifier.view());
            running.name = copy(application.name.view());
            running.not_responding = application.not_responding;
            // Unless Settings > Notifications says that application's icon has no badge.
            running.badge = badges_allowed(application.name.view()) ? copy(application.badge.view()) : String {};
            auto bundle = Desktop::application_with_identifier(application.identifier.view());
            if (bundle.has_value()) {
                running.bundle_path = copy(bundle.value().bundle_path.view());
                running.icon = copy(bundle.value().icon.view());
            } else {
                running.icon = copy("GenericApplication"_sv);
            }
            for (auto const& entry : application.windows) {
                if (entry.minimized) {
                    MinimizedEntry minimized;
                    minimized.id = entry.id;
                    minimized.title = copy(entry.title.view());
                    minimized.icon = copy("Document"_sv);
                    m_minimized_pending.append(move(minimized));
                }
            }
            m_state_applications.append(move(running));
        }
        bool changed_recents = false;
        for (auto const& path : ended) {
            if (is_kept(path.view()))
                continue;
            m_settings.recents.remove_all_matching([&](auto const& one) { return one.view() == path.view(); });
            m_settings.recents.insert(0, copy(path.view()));
            while (m_settings.recents.size() > most_recents)
                m_settings.recents.take_last();
            changed_recents = true;
        }
        if (changed_recents)
            m_settings.save();
        rebuild();
    }

    // ---- Drawing

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        compute_layout();
        if (m_hide >= 1)
            return;
        float const radius = t.metric("dock.shelfRadius"_sv);
        graphics.stroke_rounded(m_shelf, radius, graphics.pixel(), t.color("dock.border"_sv));
        float const dot = t.metric("dock.dotSize"_sv);
        double const now = Platform::the().now();
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            auto const& tile = m_tiles[i];
            if (static_cast<int>(i) == m_dragging_out)
                continue;
            if (tile.kind == Tile::Kind::Separator) {
                auto const line = separator_rect(tile);
                graphics.fill(line, t.color("dock.separator"_sv));
                continue;
            }
            float lift = 0;
            if (tile.bouncing_since >= 0) {
                double const elapsed = now - tile.bouncing_since;
                lift = static_cast<float>(Math::absolute(Math::sine(elapsed * 6))) * t.metric("dock.bounceHeight"_sv);
            }
            auto const place = icon_rect(tile, lift);
            draw_tile_picture(graphics, tile, place);
            bool const running = tile.pid != 0 && tile.kind == Tile::Kind::Application;
            if (running && m_settings.indicators)
                graphics.fill_circle(dot_center(tile), dot / 2, t.color("dock.dot"_sv));
            if (!tile.badge.is_empty())
                draw_badge(graphics, place, tile.badge.view());
        }
        if (m_hovered >= 0 && m_hovered_inside && m_dragging_out < 0 && !m_stack_open && static_cast<size_t>(m_hovered) < m_tiles.size())
            draw_label(graphics, m_tiles[static_cast<size_t>(m_hovered)]);
    }

    // ---- The pointer

    void mouse_moved(MouseEvent const& event) override
    {
        m_pointer = event.position;
        m_pointer_along = along(event.position);
        bool const inside = m_hide >= 1 ? touches_edge(event.position) : m_shelf.contains(event.position);
        if (inside != m_hovered_inside) {
            m_hovered_inside = inside;
            if (m_settings.autohide)
                set_hidden(!inside);
        }
        set_magnifying(m_hovered_inside && m_settings.magnification && m_hide < 1);
        update_hover();
        update_shape();
        set_needs_display();
    }

    void mouse_exited() override
    {
        m_hovered_inside = false;
        m_hovered = -1;
        set_magnifying(false);
        if (m_settings.autohide && !m_stack_open && !(m_menu && m_menu->is_open()))
            set_hidden(true);
        set_needs_display();
    }

    bool mouse_down(MouseEvent const& event) override
    {
        m_pointer = event.position;
        m_pointer_along = along(event.position);
        update_hover();
        if (m_hovered < 0)
            return false;
        if (event.is_right_click()) {
            show_menu(static_cast<size_t>(m_hovered));
            return false;
        }
        m_pressed = m_hovered;
        m_pressed_at = event.position;
        return true;
    }

    void mouse_dragged(MouseEvent const& event) override
    {
        m_pointer = event.position;
        m_pointer_along = along(event.position);
        if (m_pressed < 0)
            return;
        auto const& tile = m_tiles[static_cast<size_t>(m_pressed)];
        bool const movable = tile.kind == Tile::Kind::Application || tile.kind == Tile::Kind::Folder;
        if (!movable)
            return;
        // Along the Dock: put somewhere else in it. Away from it: taken out, with "Remove" over it as it goes.
        float const away = out_from_edge(event.position) - out_from_edge(m_pressed_at);
        if (away > tokens().metric("dock.removeDistance"_sv) && (tile.pinned || tile.kind == Tile::Kind::Folder)) {
            start_drag_out(static_cast<size_t>(m_pressed));
            return;
        }
        if (Math::absolute(along(event.position) - along(m_pressed_at)) > 6 && tile.pinned)
            reorder_to(static_cast<size_t>(m_pressed), event.position);
    }

    void mouse_up(MouseEvent const& event) override
    {
        m_pointer = event.position;
        update_hover();
        int const pressed = m_pressed;
        m_pressed = -1;
        if (m_reordered) {
            m_reordered = false;
            save_kept();
            return;
        }
        if (pressed < 0 || pressed != m_hovered || !m_shelf.contains(event.position))
            return;
        activate_tile(static_cast<size_t>(pressed));
    }

    // ---- What's dropped on it

    [[nodiscard]] bool accepts_drop(StringView type) const override { return type == "public.file-url"_sv || type == "com.vulture.dock-tile"_sv; }

    void drag_moved(FloatPoint point) override
    {
        m_drop_at = point;
        m_pointer_along = along(point);
    }

    void perform_drop(StringView type, ReadonlyBytes data) override
    {
        StringView const from { reinterpret_cast<char const*>(data.data()), data.size() };
        m_pointer_along = along(m_drop_at);
        update_hover();
        int const over = m_hovered;
        // A tile taken out and brought back: where it's let go.
        if (type == "com.vulture.dock-tile"_sv) {
            m_dropped_back = true;
            return;
        }
        auto const* target = over >= 0 ? &m_tiles[static_cast<size_t>(over)] : nullptr;
        if (target && target->kind == Tile::Kind::Trash) {
            StringBuilder destination;
            destination.append(Desktop::home_directory().view());
            destination.append("/.Trash/"_sv);
            destination.append(last_component(from));
            auto source = copy(from);
            auto moved_to = MUST(destination.to_string());
            (void)rename(source.characters(), moved_to.characters());
            rebuild();
            return;
        }
        // An application: kept here, where it's let go, before whatever's under it.
        if (from.ends_with(".app"_sv)) {
            if (is_kept(from))
                return;
            size_t at = m_settings.apps.size();
            if (target && target->pinned) {
                for (size_t i = 0; i < m_settings.apps.size(); ++i) {
                    if (m_settings.apps[i].view() == target->path.view())
                        at = i;
                }
            }
            m_settings.apps.insert(at, copy(from));
            m_settings.recents.remove_all_matching([&](auto const& one) { return one.view() == from; });
            save_kept();
            return;
        }
        // A folder, after the separator; a document onto an application, opened with it.
        struct stat information {};
        auto source = copy(from);
        if (stat(source.characters(), &information) == 0 && S_ISDIR(information.st_mode) && (!target || target->kind != Tile::Kind::Application)) {
            m_settings.others.append(copy(from));
            save_kept();
            return;
        }
        if (target && target->kind == Tile::Kind::Application && !target->path.is_empty())
            (void)Desktop::open_path_with(from, target->path.view());
    }

    // ---- Where it is

    void update_shape()
    {
        if (!window())
            return;
        compute_layout();
        auto const& t = tokens();
        IntRect region { static_cast<int>(m_shelf.x), static_cast<int>(m_shelf.y), static_cast<int>(m_shelf.width + 0.999f), static_cast<int>(m_shelf.height + 0.999f) };
        // Hidden, all that's left of it is the edge, for the pointer to find.
        if (m_hide >= 1) {
            region = edge_strip();
        }
        if (region == m_last_shelf)
            return;
        m_last_shelf = region;
        MaterialRegion material { m_hide >= 1 ? IntRect {} : region, WS::Material::UnderWindow, static_cast<int>(t.metric("dock.shelfRadius"_sv)) };
        window()->set_materials(Span<MaterialRegion const>(&material, m_hide >= 1 ? 0 : 1));
        window()->backend().set_input_region(region);
        // Where windows go.
        auto const origin = window()->frame().origin();
        IntPoint target { origin.x + region.right() - 40, origin.y + region.y + 20 };
        if (m_settings.edge != Edge::Bottom)
            target = { origin.x + region.x + region.width / 2, origin.y + region.bottom() - 40 };
        (void)server().set_minimize_target(target.x, target.y);
    }

    // How far into the screen the Dock comes, kept clear of windows unless it hides.
    [[nodiscard]] int reserved() const
    {
        auto const& t = tokens();
        return static_cast<int>(m_settings.tile_size + t.metric("dock.padding"_sv) * 2 + t.metric("dock.dotSize"_sv) + t.metric("dock.bottomMargin"_sv));
    }

private:
    Vector<String> m_no_badges;
    struct RunningApplication {
        i32 pid { 0 };
        String identifier;
        String name;
        String icon;
        String bundle_path;
        String badge;
        bool not_responding { false };
    };
    [[nodiscard]] static String title_of(RunningApplication const& running)
    {
        if (!running.not_responding)
            return copy(running.name.view());
        StringBuilder title;
        title.appendff("{} (Not Responding)", running.name.view());
        return MUST(title.to_string());
    }
    struct MinimizedEntry {
        u32 id;
        String title;
        String icon;
    };

    [[nodiscard]] bool is_kept(StringView bundle) const
    {
        for (auto const& path : m_settings.apps) {
            if (path.view() == bundle)
                return true;
        }
        return false;
    }

    void save_kept()
    {
        m_settings.save();
        rebuild();
    }

    // ---- Along the Dock, and out from its edge

    [[nodiscard]] bool vertical() const { return m_settings.edge != Edge::Bottom; }
    [[nodiscard]] float length() const { return vertical() ? height() : width(); }
    [[nodiscard]] float along(FloatPoint point) const { return vertical() ? point.y : point.x; }
    [[nodiscard]] float out_from_edge(FloatPoint point) const
    {
        switch (m_settings.edge) {
        case Edge::Bottom:
            return height() - point.y;
        case Edge::Left:
            return point.x;
        case Edge::Right:
            return width() - point.x;
        }
        return 0;
    }
    // A rectangle given along the Dock, and out from its edge.
    [[nodiscard]] FloatRect placed(float start, float extent, float from_edge, float depth) const
    {
        switch (m_settings.edge) {
        case Edge::Bottom:
            return { start, height() - from_edge - depth, extent, depth };
        case Edge::Left:
            return { from_edge, start, depth, extent };
        case Edge::Right:
            return { width() - from_edge - depth, start, depth, extent };
        }
        return {};
    }
    [[nodiscard]] float hidden_shift() const
    {
        auto const& t = tokens();
        return m_hide * (shelf_depth() + t.metric("dock.bottomMargin"_sv) + 4);
    }
    [[nodiscard]] float shelf_depth() const
    {
        auto const& t = tokens();
        return m_settings.tile_size + t.metric("dock.padding"_sv) * 2 + t.metric("dock.dotSize"_sv);
    }
    [[nodiscard]] bool touches_edge(FloatPoint point) const { return out_from_edge(point) <= 2; }
    [[nodiscard]] IntRect edge_strip() const
    {
        auto const strip = placed(0, length(), 0, 2);
        return { static_cast<int>(strip.x), static_cast<int>(strip.y), static_cast<int>(strip.width), static_cast<int>(strip.height) };
    }

    [[nodiscard]] FloatRect icon_rect(Tile const& tile, float lift) const
    {
        auto const& t = tokens();
        float const from_edge = t.metric("dock.bottomMargin"_sv) + t.metric("dock.padding"_sv) + t.metric("dock.dotSize"_sv) + lift - hidden_shift();
        return placed(tile.center - tile.size / 2, tile.size, from_edge, tile.size);
    }

    [[nodiscard]] FloatPoint dot_center(Tile const& tile) const
    {
        auto const& t = tokens();
        float const dot = t.metric("dock.dotSize"_sv);
        auto const spot = placed(tile.center - dot / 2, dot, t.metric("dock.bottomMargin"_sv) + 2 - hidden_shift(), dot);
        return { spot.x + dot / 2, spot.y + dot / 2 };
    }

    [[nodiscard]] FloatRect separator_rect(Tile const& tile) const
    {
        auto const& t = tokens();
        return placed(tile.center - 0.5f, 1, t.metric("dock.bottomMargin"_sv) + 8 - hidden_shift(), shelf_depth() - 16);
    }

    void draw_tile_picture(Graphics& graphics, Tile const& tile, FloatRect place)
    {
        int const scale = static_cast<int>(graphics.scale());
        // (At the size it's drawn, rounded up: a smaller one made bigger is blurred.)
        int const size = static_cast<int>(Math::ceil(static_cast<double>(place.width)));
        if (tile.kind == Tile::Kind::Folder && m_settings.stack_look == StackLook::Stack) {
            // A stack: what's newest in it, on top of what's under it.
            auto const items = items_of(tile.path.view());
            if (!items.is_empty()) {
                size_t const shown = min(items.size(), static_cast<size_t>(3));
                for (size_t i = shown; i > 0; --i) {
                    float const step = place.width * 0.06f * static_cast<float>(i - 1);
                    FloatRect const layer { place.x + step, place.y - step, place.width - step * 2, place.height - step };
                    if (auto icon = Desktop::icon_named(items[i - 1].icon.view(), size, scale))
                        graphics.painter().draw_bitmap(*icon, layer);
                }
                return;
            }
        }
        auto icon = Desktop::icon_named(tile.icon.view(), size, scale);
        if (icon)
            graphics.painter().draw_bitmap(*icon, place);
        // The Downloads folder has its arrow on it.
        if (tile.kind == Tile::Kind::Folder && last_component(tile.path.view()) == "Downloads"_sv) {
            float const mark = place.width * 0.36f;
            graphics.draw_symbol(Symbol::Download, { place.x + (place.width - mark) / 2, place.y + place.height * 0.42f, mark, mark }, tokens().color("dock.folderMark"_sv), 2);
        }
    }

    void draw_badge(Graphics& graphics, FloatRect icon, StringView text)
    {
        auto const& t = tokens();
        auto const font = t.named_font("dockBadge"_sv);
        float const size = t.metric("dock.badgeSize"_sv) * icon.width / m_settings.tile_size;
        float const text_width = Appearance::text_width(font, text);
        float const width = max(size, text_width + size * 0.6f);
        // At the top right of the icon, a little over its edge.
        FloatRect const badge { icon.x + icon.width - width + size * 0.15f, icon.y - size * 0.15f, width, size };
        graphics.fill_rounded(badge, size / 2, t.color("dock.badge"_sv));
        graphics.draw_text_centered(text, font, badge, t.color("dock.badgeText"_sv));
    }

    void draw_label(Graphics& graphics, Tile const& tile)
    {
        auto const& t = tokens();
        auto const font = t.named_font("body"_sv);
        float const label_height = t.metric("dock.labelHeight"_sv);
        float const label_width = Appearance::text_width(font, tile.title.view()) + 20;
        auto const icon = icon_rect(tile, 0);
        float const gap = t.metric("dock.labelGap"_sv);
        FloatRect label;
        switch (m_settings.edge) {
        case Edge::Bottom:
            label = { clamp(tile.center - label_width / 2, 4.0f, width() - label_width - 4), icon.y - gap - label_height, label_width, label_height };
            break;
        case Edge::Left:
            label = { icon.right() + gap, icon.y + (icon.height - label_height) / 2, label_width, label_height };
            break;
        case Edge::Right:
            label = { icon.x - gap - label_width, icon.y + (icon.height - label_height) / 2, label_width, label_height };
            break;
        }
        graphics.fill_rounded(label, 6, t.color("dock.labelBackground"_sv));
        graphics.draw_text_centered(tile.title.view(), font, label, t.color("dock.labelText"_sv));
    }

    // ---- What's on it

    void rebuild()
    {
        Vector<Tile> old = move(m_tiles);
        m_tiles = {};
        auto bouncing_since = [&](StringView identifier) -> double {
            for (auto const& tile : old) {
                if (tile.identifier.view() == identifier)
                    return tile.bouncing_since;
            }
            return -1;
        };
        auto running_of = [&](StringView bundle) -> RunningApplication const* {
            for (auto const& running : m_state_applications) {
                if (!bundle.is_empty() && running.bundle_path.view() == bundle)
                    return &running;
            }
            return nullptr;
        };
        auto application_tile = [&](StringView bundle_path) -> Optional<Tile> {
            auto bundle = Desktop::read_bundle(bundle_path);
            if (bundle.is_error())
                return {};
            Tile tile;
            tile.title = copy(bundle.value().name.view());
            tile.icon = copy(bundle.value().icon.view());
            tile.identifier = copy(bundle.value().identifier.view());
            tile.path = copy(bundle_path);
            if (auto const* running = running_of(bundle_path)) {
                tile.pid = running->pid;
                tile.badge = copy(running->badge.view());
                if (running->not_responding)
                    tile.title = title_of(*running);
            }
            tile.bouncing_since = tile.pid ? -1 : bouncing_since(tile.identifier.view());
            return tile;
        };
        for (auto const& path : m_settings.apps) {
            auto tile = application_tile(path.view());
            if (!tile.has_value())
                continue;
            tile->pinned = true;
            m_tiles.append(tile.release_value());
        }
        // What else is running, and (if wanted) what was used lately: after a separator of their own.
        Vector<Tile> others;
        for (auto const& running : m_state_applications) {
            if (is_kept(running.bundle_path.view()))
                continue;
            Tile tile;
            tile.title = title_of(running);
            tile.icon = copy(running.icon.view());
            tile.identifier = copy(running.identifier.view());
            tile.path = copy(running.bundle_path.view());
            tile.pid = running.pid;
            tile.badge = copy(running.badge.view());
            tile.recent = m_settings.show_recents;
            others.append(move(tile));
        }
        if (m_settings.show_recents) {
            for (auto const& path : m_settings.recents) {
                if (is_kept(path.view()) || running_of(path.view()) || others.size() >= most_recents)
                    continue;
                auto tile = application_tile(path.view());
                if (!tile.has_value())
                    continue;
                tile->recent = true;
                others.append(tile.release_value());
            }
            if (!others.is_empty()) {
                Tile separator;
                separator.kind = Tile::Kind::Separator;
                m_tiles.append(move(separator));
            }
        }
        for (auto& tile : others)
            m_tiles.append(move(tile));
        Tile separator;
        separator.kind = Tile::Kind::Separator;
        m_tiles.append(move(separator));
        for (auto const& folder : m_settings.others) {
            Tile tile;
            tile.kind = Tile::Kind::Folder;
            tile.title = copy(last_component(folder.view()));
            tile.icon = copy("Folder"_sv);
            tile.path = copy(folder.view());
            m_tiles.append(move(tile));
        }
        for (auto& minimized : m_minimized_pending) {
            Tile tile;
            tile.kind = Tile::Kind::MinimizedWindow;
            tile.title = move(minimized.title);
            tile.icon = move(minimized.icon);
            tile.window_id = minimized.id;
            m_tiles.append(move(tile));
        }
        m_minimized_pending.clear();
        Tile trash;
        trash.kind = Tile::Kind::Trash;
        trash.title = copy("Trash"_sv);
        trash.icon = copy(trash_has_something() ? "TrashFull"_sv : "Trash"_sv);
        m_tiles.append(move(trash));
        update_shape();
        set_needs_display();
    }

    [[nodiscard]] static bool trash_has_something()
    {
        StringBuilder path;
        path.append(Desktop::home_directory().view());
        path.append("/.Trash"_sv);
        return !items_of(path.view()).is_empty();
    }

    void reorder_to(size_t index, FloatPoint point)
    {
        // Among the ones kept, at the place under the pointer.
        m_pointer_along = along(point);
        compute_layout();
        size_t kept = 0;
        while (kept < m_tiles.size() && m_tiles[kept].pinned)
            ++kept;
        size_t target = index;
        for (size_t i = 0; i < kept; ++i) {
            if (m_pointer_along >= m_tiles[i].center - m_tiles[i].size / 2 && m_pointer_along < m_tiles[i].center + m_tiles[i].size / 2)
                target = i;
        }
        if (target == index || target >= m_settings.apps.size() || index >= m_settings.apps.size())
            return;
        auto moving = move(m_settings.apps[index]);
        m_settings.apps.remove(index);
        m_settings.apps.insert(target, move(moving));
        m_pressed = static_cast<int>(target);
        m_reordered = true;
        rebuild();
    }

    void start_drag_out(size_t index)
    {
        auto const& tile = m_tiles[index];
        m_dragging_out = static_cast<int>(index);
        m_dropped_back = false;
        auto const icon = copy(tile.icon.view());
        auto const title = copy(tile.title.view());
        float const size = m_settings.tile_size;
        auto path = copy(tile.path.view());
        window()->begin_drag("com.vulture.dock-tile"_sv, path.view().bytes(), { size + 40, size + 34 }, [icon, size](Graphics& graphics) {
            auto const& t = tokens();
            // "Remove" over it, as it'll go when it's let go.
            FloatRect const label { 0, 0, size + 40, 22 };
            graphics.fill_rounded(label, 6, t.color("dock.labelBackground"_sv));
            graphics.draw_text_centered("Remove"_sv, t.named_font("body"_sv), label, t.color("dock.labelText"_sv));
            if (auto bitmap = Desktop::icon_named(icon.view(), static_cast<int>(size), static_cast<int>(graphics.scale())))
                graphics.painter().draw_bitmap(*bitmap, FloatRect { 20, 30, size, size });
        });
        (void)title;
        m_pressed = -1;
        set_needs_display();
    }

public:
    void drag_finished(bool)
    {
        if (m_dragging_out < 0)
            return;
        // The pointer went off with what it took, and never said it had left.
        m_hovered_inside = false;
        m_hovered = -1;
        set_magnifying(false);
        auto const index = static_cast<size_t>(m_dragging_out);
        m_dragging_out = -1;
        if (m_dropped_back || index >= m_tiles.size()) {
            m_dropped_back = false;
            set_needs_display();
            return;
        }
        auto const& tile = m_tiles[index];
        auto const path = copy(tile.path.view());
        if (tile.kind == Tile::Kind::Folder)
            m_settings.others.remove_all_matching([&](auto const& one) { return one.view() == path.view(); });
        else
            m_settings.apps.remove_all_matching([&](auto const& one) { return one.view() == path.view(); });
        Platform::the().play_sound("trash"_sv);
        save_kept();
    }

private:
    void set_magnifying(bool magnifying)
    {
        if (magnifying == m_magnifying_wanted)
            return;
        m_magnifying_wanted = magnifying;
        float from = m_magnify;
        float to = magnifying ? 1 : 0;
        if (m_magnify_animation)
            motion().cancel(m_magnify_animation);
        m_magnify_animation = animate(Animation::Timing::ease_out(magnifying ? 0.12f : 0.2f), [this, from, to](float progress) {
            m_magnify = from + (to - from) * progress;
            update_shape();
            set_needs_display();
        }, [this] { m_magnify_animation = 0; });
    }

    // Auto-hide: off the edge, and back when the pointer comes to it.
    void set_hidden(bool hidden)
    {
        float const to = hidden ? 1 : 0;
        if (m_hide_target == to)
            return;
        m_hide_target = to;
        float const from = m_hide;
        if (m_hide_animation)
            motion().cancel(m_hide_animation);
        auto const& t = tokens();
        float const seconds = hidden ? t.metric("dock.hideSeconds"_sv) : t.metric("dock.showSeconds"_sv);
        m_hide_animation = animate(Animation::Timing::ease_in_out(seconds), [this, from, to](float progress) {
            m_hide = from + (to - from) * progress;
            update_shape();
            set_needs_display();
        }, [this] { m_hide_animation = 0; });
    }

    void compute_layout()
    {
        auto const& t = tokens();
        float const base = m_settings.tile_size;
        float const magnified = m_settings.magnification ? max(m_settings.large_size, base) : base;
        float const spacing = t.metric("dock.spacing"_sv);
        float const padding = t.metric("dock.padding"_sv);
        float const separator = t.metric("dock.separator"_sv);

        // First at rest, to know where each is, then swelled around the pointer.
        float total = 0;
        for (auto const& tile : m_tiles)
            total += (tile.kind == Tile::Kind::Separator ? separator : base) + spacing;
        total -= spacing;
        float const start = (length() - total) / 2;
        float at = start;
        float const range = base * 2.6f;
        for (auto& tile : m_tiles) {
            float const extent = tile.kind == Tile::Kind::Separator ? separator : base;
            float const center = at + extent / 2;
            float size = extent;
            if (tile.kind != Tile::Kind::Separator && m_magnify > 0) {
                float distance = Math::absolute(center - m_pointer_along);
                float const closeness = distance < range ? static_cast<float>(Math::cosine(static_cast<double>(distance / range) * Math::pi / 2)) : 0;
                size = base + (magnified - base) * closeness * closeness * m_magnify;
            }
            tile.size = size;
            tile.center = center;
            at += extent + spacing;
        }
        // Spread out by what they gained, from the middle.
        float grown = 0;
        for (auto const& tile : m_tiles)
            grown += tile.size - (tile.kind == Tile::Kind::Separator ? separator : base);
        float position = start - grown / 2;
        for (auto& tile : m_tiles) {
            tile.center = position + tile.size / 2;
            position += tile.size + spacing;
        }
        m_shelf = placed(start - grown / 2 - padding, total + grown + padding * 2, t.metric("dock.bottomMargin"_sv) - hidden_shift(), shelf_depth());
    }

    void update_hover()
    {
        compute_layout();
        int found = -1;
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            auto const& tile = m_tiles[i];
            if (tile.kind == Tile::Kind::Separator)
                continue;
            if (m_pointer_along >= tile.center - tile.size / 2 - 1 && m_pointer_along < tile.center + tile.size / 2 + 1)
                found = static_cast<int>(i);
        }
        m_hovered = found;
    }

    void activate_tile(size_t index)
    {
        auto& tile = m_tiles[index];
        switch (tile.kind) {
        case Tile::Kind::Separator:
            return;
        case Tile::Kind::Trash: {
            StringBuilder path;
            path.append(Desktop::home_directory().view());
            path.append("/.Trash"_sv);
            (void)Desktop::open_path(path.view());
            return;
        }
        case Tile::Kind::Folder:
            open_stack(index);
            return;
        case Tile::Kind::MinimizedWindow:
            (void)server().activate_window(tile.window_id);
            return;
        case Tile::Kind::Application:
            break;
        }
        if (tile.pid) {
            (void)server().activate_application(tile.pid);
            // Files, with nothing open, opens something.
            if (tile.identifier.view() == "com.vulture.Files"_sv && !m_files_has_windows)
                (void)Desktop::open_path(Desktop::home_directory().view());
            return;
        }
        if (tile.path.is_empty())
            return;
        printf("dock: opening %s\n", tile.title.characters());
        fflush(stdout);
        (void)Desktop::open_path(tile.path.view());
        if (motion_enabled() && m_settings.launch_animation) {
            tile.bouncing_since = Platform::the().now();
            if (m_bounce_timer < 0)
                m_bounce_timer = Platform::the().add_timer(16, true, [this] { animation_frame(); });
        }
    }

    void animation_frame()
    {
        bool any_bouncing = false;
        double const now = Platform::the().now();
        for (auto& tile : m_tiles) {
            if (tile.bouncing_since >= 0) {
                if (now - tile.bouncing_since > 3)
                    tile.bouncing_since = -1;
                else
                    any_bouncing = true;
            }
        }
        if (any_bouncing)
            set_needs_display();
        else if (m_bounce_timer >= 0) {
            Platform::the().cancel_timer(m_bounce_timer);
            m_bounce_timer = -1;
        }
    }

    // ---- Stacks

    void open_stack(size_t index)
    {
        auto const& tile = m_tiles[index];
        auto items = items_of(tile.path.view());
        auto view = m_settings.stack_view;
        if (view == StackView::Automatic)
            view = (!vertical() && items.size() <= static_cast<size_t>(tokens().metric("dock.fanMost"_sv))) ? StackView::Fan : StackView::Grid;
        if (view == StackView::Fan && vertical())
            view = StackView::Grid;
        if (view == StackView::List) {
            m_menu = adopt_own(*new Menu);
            for (auto const& item : items) {
                auto path = copy(item.path.view());
                m_menu->add(item.name.view(), [path] { (void)Desktop::open_path(path.view()); });
            }
            m_menu->add_separator();
            auto folder = copy(tile.path.view());
            m_menu->add("Open in Files"_sv, [folder] { (void)Desktop::open_path(folder.view()); });
            open_menu_at(tile);
            return;
        }
        auto const shape = view == StackView::Fan ? StackPopup::Shape::Fan : StackPopup::Shape::Grid;
        auto const size = StackPopup::size_for(shape, items.size());
        auto const icon = icon_rect(tile, 0);
        auto const origin = window()->frame().origin();
        auto const screen = Platform::the().screen_size();
        IntRect frame;
        switch (m_settings.edge) {
        case Edge::Bottom:
            frame = { static_cast<int>(static_cast<float>(origin.x) + icon.x + icon.width / 2 - (shape == StackPopup::Shape::Fan ? size.width - icon.width / 2 - tokens().metric("dock.fanPadding"_sv) : size.width / 2)), static_cast<int>(static_cast<float>(origin.y) + m_shelf.y - size.height - 6), static_cast<int>(size.width), static_cast<int>(size.height) };
            break;
        case Edge::Left:
            frame = { static_cast<int>(static_cast<float>(origin.x) + m_shelf.right() + 8), static_cast<int>(static_cast<float>(origin.y) + icon.y + icon.height / 2 - size.height / 2), static_cast<int>(size.width), static_cast<int>(size.height) };
            break;
        case Edge::Right:
            frame = { static_cast<int>(static_cast<float>(origin.x) + m_shelf.x - 8 - size.width), static_cast<int>(static_cast<float>(origin.y) + icon.y + icon.height / 2 - size.height / 2), static_cast<int>(size.width), static_cast<int>(size.height) };
            break;
        }
        frame.x = clamp(frame.x, 4, max(4, screen.width - frame.width - 4));
        frame.y = clamp(frame.y, 28, max(28, screen.height - frame.height - 4));
        close_stack();
        m_stack_window = adopt_own(*new UI::Window(tile.title.view(), frame, shape == StackPopup::Shape::Fan ? (WS::WindowBare | WS::WindowPopup) : WS::WindowPopup));
        m_stack_window->set_transparent(true);
        m_stack_window->backend().set_level(WS::LevelPopup);
        if (shape == StackPopup::Shape::Grid) {
            m_stack_window->backend().set_corner_radius(static_cast<int>(tokens().metric("dock.gridRadius"_sv)));
            MaterialRegion const material { { 0, 0, frame.width, frame.height }, WS::Material::Popover, static_cast<int>(tokens().metric("dock.gridRadius"_sv)) };
            m_stack_window->set_materials(Span<MaterialRegion const>(&material, 1));
        }
        auto& popup = m_stack_window->root_view().add<StackPopup>(shape, copy(tile.path.view()), move(items), [this] { Platform::the().defer([this] { close_stack(); }); });
        popup.set_frame({ 0, 0, static_cast<float>(frame.width), static_cast<float>(frame.height) });
        m_stack_window->set_pointer_catcher(&popup);
        m_stack_window->show();
        m_stack_window->backend().grab_pointer(true);
        m_stack_open = true;
        // A press outside it puts it away.
        m_stack_catcher = &popup;
        set_needs_display();
    }

public:
    void close_stack()
    {
        if (!m_stack_window)
            return;
        m_stack_window->backend().grab_pointer(false);
        m_stack_window->hide();
        auto* old = m_stack_window.leak_ptr();
        Platform::the().defer([old] { delete old; });
        m_stack_open = false;
        m_stack_catcher = nullptr;
        if (m_settings.autohide && !m_hovered_inside)
            set_hidden(true);
        set_needs_display();
    }

private:
    // ---- Its menus

    void open_menu_at(Tile const& tile)
    {
        auto const origin = window()->frame().origin();
        auto const size = m_menu->size();
        auto const icon = icon_rect(tile, 0);
        IntPoint at;
        switch (m_settings.edge) {
        case Edge::Bottom:
            at = { origin.x + static_cast<int>(tile.center - size.width / 2), origin.y + static_cast<int>(m_shelf.y - size.height - 8) };
            break;
        case Edge::Left:
            at = { origin.x + static_cast<int>(m_shelf.right() + 8), origin.y + static_cast<int>(icon.y) };
            break;
        case Edge::Right:
            at = { origin.x + static_cast<int>(m_shelf.x - size.width - 8), origin.y + static_cast<int>(icon.y) };
            break;
        }
        m_menu->open(*window(), at);
    }

    void show_menu(size_t index)
    {
        auto const& tile = m_tiles[index];
        m_menu = adopt_own(*new Menu);
        auto const pid = tile.pid;
        auto const path = copy(tile.path.view());
        if (tile.kind == Tile::Kind::Application) {
            // Sequoia's: its windows, then Options, then what to do with it.
            for (auto const& entry : m_state_windows_of(pid))
                m_menu->add(entry.title.view(), [id = entry.id] { (void)server().activate_window(id); });
            if (!m_state_windows_of(pid).is_empty())
                m_menu->add_separator();
            auto& options = m_menu->add_submenu("Options"_sv);
            bool const kept = tile.pinned;
            if (kept)
                options.add("Remove from Dock"_sv, [this, path] {
                    m_settings.apps.remove_all_matching([&](auto const& one) { return one.view() == path.view(); });
                    save_kept();
                });
            else
                options.add("Keep in Dock"_sv, [this, path] {
                    m_settings.apps.append(copy(path.view()));
                    m_settings.recents.remove_all_matching([&](auto const& one) { return one.view() == path.view(); });
                    save_kept();
                });
            options.add("Open at Login"_sv, [path] { Desktop::set_login_item(path.view(), !Desktop::is_login_item(path.view())); }).checked = Desktop::is_login_item(path.view());
            options.add("Show in Files"_sv, [path] { (void)Desktop::reveal_in_files(path.view()); });
            m_menu->add_separator();
            if (pid) {
                m_menu->add("Show All Windows"_sv, [pid] { (void)server().show_windows_of(pid); });
                m_menu->add("Hide"_sv, [pid] { (void)server().hide_application(pid, true); });
                m_menu->add("Quit"_sv, [pid] { (void)server().quit_application(pid); });
            } else {
                m_menu->add("Open"_sv, [this, index] { activate_tile(index); });
            }
        } else if (tile.kind == Tile::Kind::Folder) {
            auto& display = m_menu->add_submenu("Display as"_sv);
            display.add("Folder"_sv, [this] { m_settings.stack_look = StackLook::Folder; save_kept(); }).checked = m_settings.stack_look == StackLook::Folder;
            display.add("Stack"_sv, [this] { m_settings.stack_look = StackLook::Stack; save_kept(); }).checked = m_settings.stack_look == StackLook::Stack;
            auto& view = m_menu->add_submenu("View content as"_sv);
            auto look = [&](StringView title, StackView value) {
                view.add(title, [this, value] { m_settings.stack_view = value; save_kept(); }).checked = m_settings.stack_view == value;
            };
            look("Fan"_sv, StackView::Fan);
            look("Grid"_sv, StackView::Grid);
            look("List"_sv, StackView::List);
            look("Automatic"_sv, StackView::Automatic);
            m_menu->add_separator();
            auto& options = m_menu->add_submenu("Options"_sv);
            options.add("Remove from Dock"_sv, [this, path] {
                m_settings.others.remove_all_matching([&](auto const& one) { return one.view() == path.view(); });
                save_kept();
            });
            options.add("Show in Files"_sv, [path] { (void)Desktop::reveal_in_files(path.view()); });
            m_menu->add_separator();
            StringBuilder open;
            open.appendff("Open \xE2\x80\x9C{}\xE2\x80\x9D", last_component(path.view()));
            m_menu->add(open.view(), [path] { (void)Desktop::open_path(path.view()); });
        } else if (tile.kind == Tile::Kind::Trash) {
            m_menu->add("Open"_sv, [this, index] { activate_tile(index); });
        } else {
            return;
        }
        open_menu_at(tile);
    }

    [[nodiscard]] Vector<Desktop::WindowEntry> m_state_windows_of(i32 pid) const
    {
        Vector<Desktop::WindowEntry> windows;
        for (auto const& one : m_windows) {
            if (one.pid != pid)
                continue;
            Desktop::WindowEntry entry;
            entry.id = one.window.id;
            entry.title = copy(one.window.title.view());
            windows.append(move(entry));
        }
        return windows;
    }

public:
    void remember_windows(Desktop::ShellState const& state)
    {
        m_windows.clear();
        for (auto const& application : state.applications) {
            for (auto const& window : application.windows) {
                KnownWindow known;
                known.pid = application.pid;
                known.window.id = window.id;
                known.window.title = copy(window.title.view());
                m_windows.append(move(known));
            }
        }
    }

private:
    struct KnownWindow {
        i32 pid { 0 };
        Desktop::WindowEntry window;
    };

    Settings m_settings;
    Vector<Tile> m_tiles;
    Vector<KnownWindow> m_windows;
    bool m_files_has_windows { true };
    Vector<RunningApplication> m_state_applications;
    Vector<MinimizedEntry> m_minimized_pending;
    FloatRect m_shelf;
    IntRect m_last_shelf;
    FloatPoint m_pointer;
    float m_pointer_along { 0 };
    bool m_hovered_inside { false };
    int m_hovered { -1 };
    int m_pressed { -1 };
    FloatPoint m_pressed_at;
    bool m_reordered { false };
    int m_dragging_out { -1 };
    bool m_dropped_back { false };
    FloatPoint m_drop_at;
    float m_magnify { 0 };
    bool m_magnifying_wanted { false };
    Animation::Animator::Id m_magnify_animation { 0 };
    float m_hide { 0 };
    float m_hide_target { 0 };
    Animation::Animator::Id m_hide_animation { 0 };
    int m_bounce_timer { -1 };
    OwnPtr<Menu> m_menu;
    OwnPtr<UI::Window> m_stack_window;
    View* m_stack_catcher { nullptr };
    bool m_stack_open { false };
};

}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--still") == 0)
            s_still = true;
    }
    auto application_or_error = Application::create();
    if (application_or_error.is_error()) {
        fprintf(stderr, "Dock: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    if (s_still)
        set_motion_enabled(false);

    // (The theme is looked up each time: a change of appearance replaces it.)
    auto window = adopt_own(*new UI::Window("Dock"_sv, { 0, 0, 100, 100 }, WS::WindowBare | WS::WindowSticky));
    window->set_transparent(true);
    window->backend().set_level(WS::LevelDock);
    auto& dock = window->root_view().add<DockView>();
    window->on_drag_finished = [&dock](bool accepted) { dock.drag_finished(accepted); };

    // Along the bottom, or down a side under the menu bar; windows kept clear of it unless it hides.
    auto place = [&] {
        auto const screen = Platform::the().screen_size();
        int const menu_bar = static_cast<int>(tokens().metric("menubar.height"_sv));
        auto const& settings = dock.settings();
        IntRect frame;
        switch (settings.edge) {
        case Edge::Bottom: {
            int const height = static_cast<int>(tokens().metric("dock.windowHeight"_sv));
            frame = { 0, screen.height - height, screen.width, height };
            break;
        }
        case Edge::Left:
        case Edge::Right: {
            int const width = static_cast<int>(tokens().metric("dock.sideWindowWidth"_sv));
            frame = { settings.edge == Edge::Left ? 0 : screen.width - width, menu_bar, width, screen.height - menu_bar };
            break;
        }
        }
        window->set_frame(frame);
        dock.set_frame({ 0, 0, static_cast<float>(frame.width), static_cast<float>(frame.height) });
        int const reserved = settings.autohide ? 0 : dock.reserved();
        (void)server().set_work_area(menu_bar, settings.edge == Edge::Left ? reserved : 0, settings.edge == Edge::Bottom ? reserved : 0, settings.edge == Edge::Right ? reserved : 0);
        (void)server().set_dock_edge(static_cast<i32>(settings.edge), dock.reserved());
        dock.update_shape();
    };
    dock.on_placement_change = place;
    place();
    window->show();
    dock.update_shape();

    application->connection().on_displays_change = [&] { place(); };
    dock.reload_badge_settings();
    String last_state;
    application->connection().on_shell_state = [&dock, &last_state](StringView json) {
        last_state = copy(json);
        auto state = Desktop::parse_shell_state(json);
        if (state.is_error())
            return;
        dock.remember_windows(state.value());
        dock.set_state(state.value());
    };
    (void)server().subscribe_shell();

    // Others changing what it keeps (Settings, `defaults`): heard of at once.
    int notify_fd = -1;
    int notify_token = -1;
    if (notify_register_file_descriptor(preferences_changed, &notify_fd, 0, &notify_token) == NOTIFY_STATUS_OK) {
        (void)application->event_loop().watch(notify_fd, Core::EventLoop::Interest::Read, [notify_fd, &dock] {
            u8 token[4];
            (void)read(notify_fd, token, sizeof(token));
            dock.reload_settings();
        });
    }
    // Settings > Notifications: badges shown again, or not, at once.
    int badges_fd = -1;
    int badges_token = -1;
    if (notify_register_file_descriptor("com.vulture.notifications.preferences", &badges_fd, 0, &badges_token) == NOTIFY_STATUS_OK) {
        (void)application->event_loop().watch(badges_fd, Core::EventLoop::Interest::Read, [badges_fd, &dock, &last_state] {
            u8 token[4];
            (void)read(badges_fd, token, sizeof(token));
            dock.reload_badge_settings();
            auto state = Desktop::parse_shell_state(last_state.view());
            if (!state.is_error())
                dock.set_state(state.value());
        });
    }
    // Option-Command-D: hide it, and show it again.
    (void)server().register_hotkey(KeyD, WS::ModifierCommand | WS::ModifierOption);
    application->connection().on_hotkey = [&dock](u16 code, u32) {
        if (code == KeyD)
            dock.toggle_autohide();
    };
    printf("dock: ready\n");
    fflush(stdout);
    return application->exec();
}
