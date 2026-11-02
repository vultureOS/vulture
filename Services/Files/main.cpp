/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <FilesEndpoint.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/Layout.h>
#include <LibUI/Menu.h>
#include <LibUI/Motion.h>
#include <LibUI/Panels.h>
#include <LibUI/ScrollView.h>
#include <LibUI/TableView.h>
#include <LibUI/TextField.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Files: folders as windows, and the desktop as a folder under them all. There is one
// of it, which others reach at com.vulture.Files to show them a folder.

using namespace UI;
using namespace Gfx;

namespace {

bool s_still;
constexpr StringView s_file_type = "public.file-url"_sv;

Appearance const& tokens()
{
    return Appearance::the();
}

String joined(StringView directory, StringView name)
{
    StringBuilder builder;
    builder.append(directory);
    if (!directory.ends_with('/'))
        builder.append("/"_sv);
    builder.append(name);
    return MUST(builder.to_string());
}

StringView last_component(StringView path)
{
    for (size_t i = path.length(); i > 0; --i) {
        if (path[i - 1] == '/' && i != path.length())
            return path.substring_view(i, path.length() - i);
    }
    return path;
}

String parent_of(StringView path)
{
    size_t end = path.length();
    while (end > 1 && path[end - 1] != '/')
        --end;
    if (end > 1)
        --end;
    return MUST(String::from_view(path.substring_view(0, end)));
}

struct FileItem {
    String name;
    String path;
    String icon;
    bool is_directory { false };
    u64 size { 0 };
    i64 modified { 0 };
    bool selected { false };
};

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

bool sorts_before(StringView a, StringView b)
{
    size_t common = min(a.length(), b.length());
    for (size_t i = 0; i < common; ++i) {
        char x = lower(a[i]), y = lower(b[i]);
        if (x != y)
            return x < y;
    }
    return a.length() < b.length();
}

Vector<FileItem> list_folder(StringView path, bool show_hidden = false)
{
    Vector<FileItem> items;
    auto terminated = MUST(String::from_view(path));
    DIR* directory = opendir(terminated.characters());
    if (!directory)
        return items;
    while (auto* entry = readdir(directory)) {
        StringView name { entry->d_name, strlen(entry->d_name) };
        if (name == "."_sv || name == ".."_sv)
            continue;
        if (!show_hidden && name.starts_with('.'))
            continue;
        FileItem item;
        item.name = MUST(String::from_view(name));
        item.path = joined(path, name);
        struct stat info;
        if (stat(item.path.characters(), &info) == 0) {
            item.is_directory = S_ISDIR(info.st_mode);
            item.size = static_cast<u64>(info.st_size);
            item.modified = static_cast<i64>(info.st_mtime);
        }
        item.icon = Desktop::icon_name_for_path(item.path.view());
        items.append(move(item));
    }
    closedir(directory);
    for (size_t i = 1; i < items.size(); ++i) {
        for (size_t j = i; j > 0 && sorts_before(items[j].name.view(), items[j - 1].name.view()); --j) {
            auto moved = move(items[j]);
            items[j] = move(items[j - 1]);
            items[j - 1] = move(moved);
        }
    }
    return items;
}

// ---- What can be done to files

ErrorOr<void> copy_file(StringView from, StringView to)
{
    auto source_path = MUST(String::from_view(from));
    auto target_path = MUST(String::from_view(to));
    struct stat info;
    if (lstat(source_path.characters(), &info) != 0)
        return Error::from_errno(errno);
    if (S_ISDIR(info.st_mode)) {
        if (mkdir(target_path.characters(), info.st_mode & 07777) != 0 && errno != EEXIST)
            return Error::from_errno(errno);
        for (auto const& item : list_folder(from, true))
            TRY(copy_file(item.path.view(), joined(to, item.name.view()).view()));
        return {};
    }
    int in = open(source_path.characters(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return Error::from_errno(errno);
    int out = open(target_path.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, info.st_mode & 07777);
    if (out < 0) {
        int saved = errno;
        close(in);
        return Error::from_errno(saved);
    }
    static char buffer[65536];
    ssize_t got;
    while ((got = read(in, buffer, sizeof(buffer))) > 0) {
        if (write(out, buffer, static_cast<size_t>(got)) != got) {
            int saved = errno;
            close(in);
            close(out);
            return Error::from_errno(saved);
        }
    }
    close(in);
    close(out);
    return {};
}

ErrorOr<void> remove_tree(StringView path)
{
    auto terminated = MUST(String::from_view(path));
    struct stat info;
    if (lstat(terminated.characters(), &info) != 0)
        return Error::from_errno(errno);
    if (S_ISDIR(info.st_mode)) {
        for (auto const& item : list_folder(path, true))
            TRY(remove_tree(item.path.view()));
        if (rmdir(terminated.characters()) != 0)
            return Error::from_errno(errno);
        return {};
    }
    if (unlink(terminated.characters()) != 0)
        return Error::from_errno(errno);
    return {};
}

bool exists(StringView path)
{
    auto terminated = MUST(String::from_view(path));
    struct stat info;
    return lstat(terminated.characters(), &info) == 0;
}

// A name not yet used in the folder: "untitled folder", "untitled folder 2", ...
String free_name(StringView directory, StringView wanted)
{
    if (!exists(joined(directory, wanted).view()))
        return MUST(String::from_view(wanted));
    for (int i = 2;; ++i) {
        StringBuilder builder;
        builder.append(wanted);
        builder.appendff(" {}", i);
        auto candidate = MUST(builder.to_string());
        if (!exists(joined(directory, candidate.view()).view()))
            return candidate;
    }
}

ErrorOr<void> move_item(StringView from, StringView to)
{
    auto a = MUST(String::from_view(from));
    auto b = MUST(String::from_view(to));
    if (rename(a.characters(), b.characters()) == 0)
        return {};
    // Across volumes: copied, then gone.
    TRY(copy_file(from, to));
    return remove_tree(from);
}

// What was last copied: Files's own pasteboard, of whole files.
Vector<String> s_copied_paths;

// ---- The icons of a folder

// A name being edited: Return and Escape end it, and go no further.
class RenameField final : public TextField {
public:
    RenameField()
        : TextField({}, FieldStyle::Rounded)
    {
    }

    Function<void()> on_cancel;

    bool key_down(KeyEvent const& event) override
    {
        if (event.code == KeyEscape) {
            if (on_cancel)
                on_cancel();
            return true;
        }
        if (event.code == KeyEnter) {
            (void)TextField::key_down(event);
            return true;
        }
        return TextField::key_down(event);
    }
};

class FolderView : public View {
public:
    explicit FolderView(bool on_desktop)
        : m_on_desktop(on_desktop)
    {
    }

    Function<void(StringView)> on_open_folder;
    Function<void()> on_changed;

    void set_folder(StringView path)
    {
        m_path = MUST(String::from_view(path));
        reload();
    }

    [[nodiscard]] StringView path() const { return m_path.view(); }
    [[nodiscard]] Vector<FileItem> const& items() const { return m_items; }

    void reload()
    {
        m_items = list_folder(m_path.view(), m_path.view().ends_with("/.Trash"_sv));
        if (parent())
            parent()->set_needs_layout();
        set_needs_layout();
        set_needs_display();
        if (on_changed)
            on_changed();
    }

    [[nodiscard]] Vector<String> selected_paths() const
    {
        Vector<String> paths;
        for (auto const& item : m_items) {
            if (item.selected)
                paths.append(MUST(String::from_view(item.path.view())));
        }
        return paths;
    }

    void clear_selection()
    {
        for (auto& item : m_items)
            item.selected = false;
        set_needs_display();
    }

    void select_named(StringView name)
    {
        clear_selection();
        for (auto& item : m_items) {
            if (item.name.view() == name)
                item.selected = true;
        }
        set_needs_display();
    }

    void new_folder()
    {
        auto name = free_name(m_path.view(), "untitled folder"_sv);
        auto full = joined(m_path.view(), name.view());
        if (mkdir(full.characters(), 0755) != 0)
            return;
        reload();
        select_named(name.view());
        begin_renaming();
    }

    void move_selection_to_trash()
    {
        auto trash = joined(Desktop::home_directory().view(), ".Trash"_sv);
        mkdir(trash.characters(), 0755);
        auto paths = selected_paths();
        for (auto const& path : paths) {
            auto name = free_name(trash.view(), last_component(path.view()));
            (void)move_item(path.view(), joined(trash.view(), name.view()).view());
        }
        if (!paths.is_empty())
            Platform::the().play_sound("trash"_sv);
        reload();
    }

    void copy_selection()
    {
        s_copied_paths = selected_paths();
    }

    void paste()
    {
        for (auto const& path : s_copied_paths) {
            auto name = free_name(m_path.view(), last_component(path.view()));
            (void)copy_file(path.view(), joined(m_path.view(), name.view()).view());
        }
        reload();
    }

    void open_selection()
    {
        for (auto const& path : selected_paths())
            open_item(path.view());
    }

    void begin_renaming()
    {
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (!m_items[i].selected)
                continue;
            auto const& t = tokens();
            auto rect = item_rect(i);
            float height_of_field = 18;
            m_rename_index = static_cast<int>(i);
            if (!m_rename_field) {
                m_rename_field = &add<RenameField>();
                m_rename_field->on_commit = [this] { finish_renaming(); };
                m_rename_field->on_cancel = [this] {
                    m_rename_field->set_text(m_items[static_cast<size_t>(m_rename_index)].name.view());
                    finish_renaming();
                };
            }
            m_rename_field->set_hidden(false);
            m_rename_field->set_text(m_items[i].name.view());
            m_rename_field->set_frame({ rect.x, rect.y + t.metric("files.iconSize"_sv) + 6, rect.width, height_of_field });
            if (window()) {
                window()->make_first_responder(m_rename_field);
                m_rename_field->select_all();
            }
            return;
        }
    }

    [[nodiscard]] FloatSize intrinsic_size() const override
    {
        auto const& t = tokens();
        if (m_on_desktop)
            return { -1, -1 };
        int across = columns();
        int rows = (static_cast<int>(m_items.size()) + across - 1) / across;
        return { -1, static_cast<float>(rows) * t.metric("files.cellHeight"_sv) + 20 };
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        if (!m_on_desktop)
            graphics.fill({ 0, 0, width(), height() }, t.color("textBackground"_sv));
        if (m_drop_target)
            graphics.fill({ 0, 0, width(), height() }, t.color("files.selection"_sv));
        auto font = t.named_font("small"_sv);
        font.size = 12;
        float icon_size = t.metric("files.iconSize"_sv);
        int scale = static_cast<int>(graphics.scale());
        float cap = Appearance::cap_height(font);
        for (size_t i = 0; i < m_items.size(); ++i) {
            auto const& item = m_items[i];
            auto rect = item_rect(i);
            float icon_x = rect.x + (rect.width - icon_size) / 2;
            if (item.selected)
                graphics.fill_rounded({ icon_x - 4, rect.y - 3, icon_size + 8, icon_size + 6 }, 5, t.color("files.selection"_sv));
            auto icon = Desktop::icon_named(item.icon.view(), static_cast<int>(icon_size), scale);
            if (icon)
                graphics.painter().draw_bitmap(*icon, FloatRect { icon_x, rect.y, icon_size, icon_size });
            if (static_cast<int>(i) == m_rename_index)
                continue;
            StringView shown = item.name.view();
            if (shown.ends_with(".app"_sv))
                shown = shown.substring_view(0, shown.length() - 4);
            float label_width = min(Appearance::text_width(font, shown), rect.width - 4);
            FloatRect label { rect.x + (rect.width - label_width) / 2 - 4, rect.y + icon_size + 5, label_width + 8, 16 };
            if (item.selected)
                graphics.fill_rounded(label, 4, t.color(window() && window()->is_key() ? "selectedContent"_sv : "selectedContentInactive"_sv));
            auto text_color = item.selected && window() && window()->is_key() ? t.color("selectedContentText"_sv) : (m_on_desktop ? t.color("files.desktopText"_sv) : t.color("label"_sv));
            graphics.draw_text_truncated(shown, font, { label.x + 4, label.y + (label.height + cap) / 2 }, label_width, text_color);
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        if (m_rename_index >= 0)
            finish_renaming();
        int hit = item_at(event.position);
        if (event.is_right_click()) {
            show_context_menu(event, hit);
            return false;
        }
        if (event.button != WS::ButtonLeft)
            return false;
        window()->make_first_responder(this);
        if (hit < 0) {
            clear_selection();
            if (on_changed)
                on_changed();
            return true;
        }
        auto& item = m_items[static_cast<size_t>(hit)];
        bool extend = event.modifiers & (WS::ModifierCommand | WS::ModifierShift);
        if (!item.selected || !extend) {
            if (!extend && !item.selected)
                clear_selection();
            if (extend)
                item.selected = !item.selected;
            else
                item.selected = true;
        } else if (extend) {
            item.selected = false;
        }
        set_needs_display();
        if (on_changed)
            on_changed();
        if (event.click_count == 2) {
            open_item(item.path.view());
            return true;
        }
        m_press_index = hit;
        m_press_at = event.window_position;
        return true;
    }

    void mouse_dragged(MouseEvent const& event) override
    {
        if (m_press_index < 0 || !window())
            return;
        auto moved = event.window_position - m_press_at;
        float reach = window()->click_distance();
        if (moved.x * moved.x + moved.y * moved.y < reach * reach)
            return;
        auto const& item = m_items[static_cast<size_t>(m_press_index)];
        m_press_index = -1;
        auto icon_name = MUST(String::from_view(item.icon.view()));
        float size = tokens().metric("files.iconSize"_sv);
        window()->begin_drag(s_file_type, ReadonlyBytes(reinterpret_cast<u8 const*>(item.path.characters()), item.path.length()), { size, size }, [icon_name = move(icon_name), size](Graphics& graphics) {
            auto icon = Desktop::icon_named(icon_name.view(), static_cast<int>(size), static_cast<int>(graphics.scale()));
            if (icon)
                graphics.painter().draw_bitmap(*icon, FloatRect { 0, 0, size, size }, 0.8f);
        });
    }

    void mouse_up(MouseEvent const&) override { m_press_index = -1; }

    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    [[nodiscard]] bool shows_focus_ring() const override { return false; }

    bool key_down(KeyEvent const& event) override
    {
        if (event.has_command()) {
            switch (event.code) {
            case KeyDown:
            case KeyO:
                open_selection();
                return true;
            case KeyBackspace:
                move_selection_to_trash();
                return true;
            case KeyC:
                copy_selection();
                return true;
            case KeyV:
                paste();
                return true;
            case KeyUp:
                if (on_open_folder && m_path.view() != "/"_sv)
                    on_open_folder(parent_of(m_path.view()).view());
                return true;
            case KeyA:
                for (auto& item : m_items)
                    item.selected = true;
                set_needs_display();
                return true;
            default:
                return false;
            }
        }
        if (event.code == KeyEnter) {
            begin_renaming();
            return true;
        }
        int across = columns();
        int by = 0;
        switch (event.code) {
        case KeyLeft:
            by = m_on_desktop ? -1000 : -1;
            break;
        case KeyRight:
            by = m_on_desktop ? 1000 : 1;
            break;
        case KeyUp:
            by = m_on_desktop ? -1 : -across;
            break;
        case KeyDown:
            by = m_on_desktop ? 1 : across;
            break;
        default:
            return false;
        }
        if (m_items.is_empty())
            return true;
        int current = -1;
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (m_items[i].selected)
                current = static_cast<int>(i);
        }
        int next = clamp(current < 0 ? 0 : current + by, 0, static_cast<int>(m_items.size()) - 1);
        clear_selection();
        m_items[static_cast<size_t>(next)].selected = true;
        set_needs_display();
        if (on_changed)
            on_changed();
        return true;
    }

    [[nodiscard]] bool accepts_drop(StringView type) const override { return type == s_file_type; }
    void drag_entered() override
    {
        m_drop_target = true;
        set_needs_display();
    }
    void drag_exited() override
    {
        m_drop_target = false;
        set_needs_display();
    }
    void perform_drop(StringView, ReadonlyBytes data) override
    {
        m_drop_target = false;
        StringView from { reinterpret_cast<char const*>(data.data()), data.size() };
        auto name = last_component(from);
        auto target = joined(m_path.view(), name);
        if (target.view() == from)
            return;
        auto free = free_name(m_path.view(), name);
        (void)move_item(from, joined(m_path.view(), free.view()).view());
        reload();
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Table; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(last_component(m_path.view()))); }

private:
    [[nodiscard]] int columns() const
    {
        return max(static_cast<int>(width() / tokens().metric("files.cellWidth"_sv)), 1);
    }

    [[nodiscard]] FloatRect item_rect(size_t index) const
    {
        auto const& t = tokens();
        float cell_width = t.metric("files.cellWidth"_sv);
        if (m_on_desktop) {
            // Down the right side of the screen, top first.
            float cell_height = t.metric("files.desktopCellHeight"_sv);
            auto per_column = max(static_cast<int>((height() - 20) / cell_height), 1);
            int column = static_cast<int>(index) / per_column;
            int row = static_cast<int>(index) % per_column;
            return { width() - cell_width * static_cast<float>(column + 1) - 8, 12 + static_cast<float>(row) * cell_height, cell_width, cell_height };
        }
        float cell_height = t.metric("files.cellHeight"_sv);
        int across = columns();
        int column = static_cast<int>(index) % across;
        int row = static_cast<int>(index) / across;
        return { static_cast<float>(column) * cell_width + 4, static_cast<float>(row) * cell_height + 12, cell_width, cell_height };
    }

    [[nodiscard]] int item_at(FloatPoint point) const
    {
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (item_rect(i).contains(point))
                return static_cast<int>(i);
        }
        return -1;
    }

    void open_item(StringView path)
    {
        struct stat info;
        auto terminated = MUST(String::from_view(path));
        bool is_directory = stat(terminated.characters(), &info) == 0 && S_ISDIR(info.st_mode);
        if (is_directory && !path.ends_with(".app"_sv)) {
            if (on_open_folder)
                on_open_folder(path);
            return;
        }
        (void)Desktop::open_path(path);
    }

    void finish_renaming()
    {
        if (m_rename_index < 0 || !m_rename_field)
            return;
        auto index = static_cast<size_t>(m_rename_index);
        m_rename_index = -1;
        auto wanted = MUST(String::from_view(m_rename_field->text()));
        m_rename_field->set_hidden(true);
        if (index < m_items.size() && !wanted.is_empty() && wanted.view() != m_items[index].name.view()) {
            auto target = joined(m_path.view(), wanted.view());
            (void)rename(m_items[index].path.characters(), target.characters());
        }
        reload();
        select_named(wanted.view());
        if (window())
            window()->make_first_responder(this);
    }

    void show_context_menu(MouseEvent const& event, int hit)
    {
        m_menu = adopt_own(*new Menu);
        if (hit >= 0) {
            auto& item = m_items[static_cast<size_t>(hit)];
            if (!item.selected) {
                clear_selection();
                item.selected = true;
            }
            m_menu->add("Open"_sv, [this] { open_selection(); });
            m_menu->add_separator();
            m_menu->add("Move to Trash"_sv, [this] { move_selection_to_trash(); });
            m_menu->add_separator();
            m_menu->add("Rename"_sv, [this] { begin_renaming(); });
            m_menu->add("Copy"_sv, [this] { copy_selection(); });
        } else {
            m_menu->add("New Folder"_sv, [this] { new_folder(); });
            auto& paste_item = m_menu->add("Paste"_sv, [this] { paste(); });
            paste_item.enabled = !s_copied_paths.is_empty();
        }
        auto origin = window()->frame().origin();
        m_menu->open(*window(), { origin.x + static_cast<int>(event.window_position.x), origin.y + static_cast<int>(event.window_position.y) });
    }

    bool m_on_desktop;
    String m_path;
    Vector<FileItem> m_items;
    RenameField* m_rename_field { nullptr };
    int m_rename_index { -1 };
    int m_press_index { -1 };
    FloatPoint m_press_at;
    bool m_drop_target { false };
    OwnPtr<Menu> m_menu;
};

// ---- The same, as a list

class ListModel final : public TableModel {
public:
    void set_items(Vector<FileItem> const* items) { m_items = items; }
    [[nodiscard]] size_t row_count() const override { return m_items ? m_items->size() : 0; }
    [[nodiscard]] String text_at(size_t row, size_t column) const override
    {
        auto const& item = (*m_items)[row];
        StringBuilder builder;
        switch (column) {
        case 0:
            builder.append(item.name.view());
            break;
        case 1:
            if (item.is_directory) {
                builder.append("--"_sv);
            } else if (item.size < 1024) {
                builder.appendff("{} bytes", item.size);
            } else if (item.size < 1024 * 1024) {
                builder.appendff("{} KB", item.size / 1024);
            } else {
                builder.appendff("{} MB", item.size / (1024 * 1024));
            }
            break;
        default:
            builder.append(item.is_directory ? "Folder"_sv : "Document"_sv);
            break;
        }
        return MUST(builder.to_string());
    }
    [[nodiscard]] bool has_symbol_at(size_t) const override { return true; }
    [[nodiscard]] Symbol symbol_at(size_t row) const override { return (*m_items)[row].is_directory ? Symbol::Folder : Symbol::File; }

private:
    Vector<FileItem> const* m_items { nullptr };
};

class FilesWindow;

class FilesController {
public:
    void open_new_window(StringView path);
    void window_closed(FilesWindow&);
    [[nodiscard]] FilesWindow* key_window();

private:
    Vector<NonnullOwnPtr<FilesWindow>> m_windows;
};

class FilesContent final : public View {
public:
    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float sidebar = t.metric("files.sidebarWidth"_sv);
        graphics.fill({ sidebar, 0, width() - sidebar, height() }, t.color("windowBackground"_sv));
    }
};

class FilesWindow {
public:
    FilesWindow(FilesController& controller, StringView path, IntPoint at)
        : m_controller(controller)
    {
        auto const& t = tokens();
        m_window = adopt_own(*new UI::Window(last_component(path), { at.x, at.y, 860, 520 }, WS::WindowTitled | WS::WindowResizable | WS::WindowUnifiedTitle));
        m_window->set_transparent(true);
        m_window->set_sheet_top(t.metric("window.toolbarUnified"_sv));
        auto& root = m_window->set_content<FilesContent>();
        auto& split = root.add<SplitView>();
        m_split = &split;
        float sidebar_width = t.metric("files.sidebarWidth"_sv);
        m_sidebar = &split.add_pane<Sidebar>(sidebar_width, sidebar_width);
        m_sidebar->set_top_padding(t.metric("window.toolbarCompact"_sv));
        auto& right = split.add_pane<StackView>(0, 300, Axis::Vertical);
        m_toolbar = &right.add<Toolbar>();
        m_toolbar->set_left_padding(t.metric("toolbar.paddingRight"_sv));
        m_back = &m_toolbar->add_button(Symbol::ChevronLeft, [this] { go_back(); }, "Back"_sv);
        m_forward = &m_toolbar->add_button(Symbol::ChevronRight, [this] { go_forward(); }, "Forward"_sv);
        m_title = &m_toolbar->add_item<Label>(last_component(path), t.named_font("headline"_sv));
        m_toolbar->add_flexible_space();
        m_view_mode = &m_toolbar->add_segmented();
        m_view_mode->add_segment("Icons"_sv);
        m_view_mode->add_segment("List"_sv);
        m_view_mode->set_selected(0);
        m_view_mode->on_change = [this](size_t) { update_view_mode(); };
        (void)m_toolbar->add_button(Symbol::Folder, [this] { m_icons->new_folder(); }, "New Folder"_sv);
        m_scroll = &right.add<ScrollView>();
        m_scroll->set_stretch(0, 1);
        m_scroll->set_scrolls_horizontally(false);
        m_icons = &m_scroll->make_document<FolderView>(false);
        m_icons->on_open_folder = [this](StringView folder) { navigate(folder, true); };
        m_icons->on_changed = [this] {
            update_status();
            m_table->reload();
        };
        m_table = &right.add<TableView>(&m_list_model);
        m_table->add_column("Name"_sv, 340);
        m_table->add_column("Size"_sv, 100);
        m_table->add_column("Kind"_sv, 120);
        m_table->set_stretch(0, 1);
        m_table->set_hidden(true);
        m_table->on_activate = [this](size_t row) { open_list_row(row); };
        m_status = &right.add<Label>(StringView {}, t.named_font("small"_sv));
        m_status->set_style(LabelStyle::Secondary);
        m_status->set_alignment(TextAlignment::Center);
        m_status->set_fixed_height(22);

        auto home = Desktop::home_directory();
        m_sidebar->add_section("Favorites"_sv);
        add_place("Home"_sv, home.view(), Symbol::Home);
        add_place("Desktop"_sv, joined(home.view(), "Desktop"_sv).view(), Symbol::Desktop);
        add_place("Documents"_sv, joined(home.view(), "Documents"_sv).view(), Symbol::File);
        add_place("Downloads"_sv, joined(home.view(), "Downloads"_sv).view(), Symbol::Download);
        add_place("Applications"_sv, "/Applications"_sv, Symbol::Applications);
        m_sidebar->add_section("Locations"_sv);
        add_place("Vulture HD"_sv, "/"_sv, Symbol::Drive);
        add_place("Trash"_sv, joined(home.view(), ".Trash"_sv).view(), Symbol::Trash);
        m_sidebar->on_select = [this](size_t row) {
            if (m_syncing_sidebar)
                return;
            navigate(m_places[row].view(), true);
        };

        m_window->on_resize = [this](FloatSize size) {
            m_split->set_frame({ 0, 0, size.width, size.height });
            update_material();
        };
        split.set_frame({ 0, 0, 860, 520 });
        update_material();
        m_window->on_closed = [this] { m_controller.window_closed(*this); };
        navigate(path, false);
        m_window->show();
    }

    [[nodiscard]] UI::Window& window() { return *m_window; }
    [[nodiscard]] FolderView& icons() { return *m_icons; }
    void navigate_to(StringView path) { navigate(path, true); }

    void go_back()
    {
        if (m_back_stack.is_empty())
            return;
        m_forward_stack.append(MUST(String::from_view(m_icons->path())));
        auto target = m_back_stack.take_last();
        navigate(target.view(), false);
    }

    void go_forward()
    {
        if (m_forward_stack.is_empty())
            return;
        m_back_stack.append(MUST(String::from_view(m_icons->path())));
        auto target = m_forward_stack.take_last();
        navigate(target.view(), false);
    }

private:
    void add_place(StringView title, StringView path, Symbol symbol)
    {
        m_sidebar->add_row(title, symbol);
        m_places.append(MUST(String::from_view(path)));
    }

    void navigate(StringView given, bool remember)
    {
        // (What it was given may be in the folder's list, which is about to change.)
        auto owned = MUST(String::from_view(given));
        auto path = owned.view();
        if (remember) {
            m_back_stack.append(MUST(String::from_view(m_icons->path())));
            m_forward_stack.clear();
        }
        m_icons->set_folder(path);
        m_list_model.set_items(&m_icons->items());
        m_table->reload();
        StringView shown = path == "/"_sv ? "Vulture HD"_sv : last_component(path);
        if (path.ends_with("/.Trash"_sv))
            shown = "Trash"_sv;
        m_window->set_title(shown);
        m_title->set_text(shown);
        m_back->set_enabled(!m_back_stack.is_empty());
        m_forward->set_enabled(!m_forward_stack.is_empty());
        m_syncing_sidebar = true;
        for (size_t i = 0; i < m_places.size(); ++i) {
            if (m_places[i].view() == path)
                m_sidebar->select_row(i);
        }
        m_syncing_sidebar = false;
        m_scroll->scroll_to({ 0, 0 });
        update_status();
    }

    void update_status()
    {
        size_t count = m_icons->items().size();
        size_t selected = m_icons->selected_paths().size();
        StringBuilder builder;
        if (selected > 0)
            builder.appendff("{} of {} selected", selected, count);
        else
            builder.appendff("{} item{}", count, count == 1 ? "" : "s");
        m_status->set_text(builder.view());
    }

    void update_view_mode()
    {
        bool list = m_view_mode->selected_index() == 1;
        m_scroll->set_hidden(list);
        m_table->set_hidden(!list);
    }

    void open_list_row(size_t row)
    {
        auto const& item = m_icons->items()[row];
        if (item.is_directory && !item.path.view().ends_with(".app"_sv)) {
            auto path = MUST(String::from_view(item.path.view()));
            navigate(path.view(), true);
        } else {
            (void)Desktop::open_path(item.path.view());
        }
    }

    void update_material()
    {
        auto const& t = tokens();
        MaterialRegion region { { 0, 0, static_cast<int>(t.metric("files.sidebarWidth"_sv)), static_cast<int>(m_window->size().height) }, WS::Material::Sidebar };
        m_window->set_materials(Span<MaterialRegion const>(&region, 1));
    }

    FilesController& m_controller;
    OwnPtr<UI::Window> m_window;
    SplitView* m_split { nullptr };
    Sidebar* m_sidebar { nullptr };
    Toolbar* m_toolbar { nullptr };
    ToolbarButton* m_back { nullptr };
    ToolbarButton* m_forward { nullptr };
    Label* m_title { nullptr };
    SegmentedControl* m_view_mode { nullptr };
    ScrollView* m_scroll { nullptr };
    FolderView* m_icons { nullptr };
    TableView* m_table { nullptr };
    Label* m_status { nullptr };
    ListModel m_list_model;
    Vector<String> m_places;
    Vector<String> m_back_stack;
    Vector<String> m_forward_stack;
    bool m_syncing_sidebar { false };
};

void FilesController::open_new_window(StringView path)
{
    int offset = static_cast<int>(m_windows.size()) * 28;
    m_windows.append(adopt_own(*new FilesWindow(*this, path, { 180 + offset, 80 + offset })));
}

void FilesController::window_closed(FilesWindow& window)
{
    // After this event is done with it.
    Platform::the().defer([this, &window] {
        for (size_t i = 0; i < m_windows.size(); ++i) {
            if (m_windows[i].ptr() == &window) {
                m_windows.remove(i);
                return;
            }
        }
    });
}

FilesWindow* FilesController::key_window()
{
    for (auto& window : m_windows) {
        if (window->window().is_key())
            return window.ptr();
    }
    return nullptr;
}

class FilesService final : public FilesStub {
public:
    explicit FilesService(FilesController& controller)
        : m_controller(controller)
    {
    }

    void open_folder(IPC::Sender const&, String path) override { m_controller.open_new_window(path.view()); }

private:
    FilesController& m_controller;
};

}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    StringView folder;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--still") == 0)
            s_still = true;
        else
            folder = StringView(argv[i], strlen(argv[i]));
    }
    // There is one of it.
    if (auto running = IPC::look_up("com.vulture.Files"_sv); !running.is_error()) {
        if (!folder.is_empty())
            (void)FilesProxy(running.release_value()).open_folder(folder);
        return 0;
    }
    auto application_or_error = Application::create("Files"_sv, "com.vulture.Files"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Files: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    application->set_quits_with_last_window(false);
    if (s_still)
        set_motion_enabled(false);

    FilesController controller;
    FilesService service(controller);
    OwnPtr<IPC::Listener> listener;
    auto pair = IPC::create_port();
    if (!pair.is_error()) {
        auto ports = pair.release_value();
        listener = adopt_own(*new IPC::Listener(application->event_loop(), move(ports.receive), [&](IPC::ReceivedMessage& message) { (void)service.handle(message); }));
        (void)IPC::register_service("com.vulture.Files"_sv, move(ports.send));
    }

    auto home = Desktop::home_directory();
    auto desktop_path = joined(home.view(), "Desktop"_sv);

    // The desktop: under everything.
    auto screen = Platform::the().screen_size();
    auto bar = static_cast<int>(tokens().metric("menubar.height"_sv));
    auto desktop_window = adopt_own(*new UI::Window("Desktop"_sv, { 0, bar, screen.width, screen.height - bar }, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
    desktop_window->set_transparent(true);
    desktop_window->backend().set_level(WS::LevelDesktop);
    auto& desktop = desktop_window->set_content<FolderView>(true);
    desktop.set_folder(desktop_path.view());
    desktop.on_open_folder = [&controller](StringView path) { controller.open_new_window(path); };
    desktop_window->show();

    if (folder.is_empty())
        controller.open_new_window(home.view());
    else
        controller.open_new_window(folder);

    // What is under Files's name in the menu bar.
    // Part of the desktop: there is no quitting it.
    auto menus = application->standard_menus(nullptr, false);
    application->on_quit_request = [] { return false; };
    auto selected_view = [&]() -> FolderView* {
        if (auto* window = controller.key_window())
            return &window->icons();
        return desktop_window->is_key() ? &desktop : nullptr;
    };
    auto& file = *new Menu("File"_sv);
    file.add_with_shortcut("New Files Window"_sv, KeyN, "\xE2\x8C\x98N"_sv, [&controller, &home] { controller.open_new_window(home.view()); });
    file.add_with_shortcut("New Folder"_sv, KeyN, "\xE2\x87\xA7\xE2\x8C\x98N"_sv, [selected_view] {
        if (auto* view = selected_view())
            view->new_folder();
    },
        WS::ModifierCommand | WS::ModifierShift);
    file.add_with_shortcut("Open"_sv, KeyO, "\xE2\x8C\x98O"_sv, [selected_view] {
        if (auto* view = selected_view())
            view->open_selection();
    });
    file.add_with_shortcut("Close Window"_sv, KeyW, "\xE2\x8C\x98W"_sv, [desktop = desktop_window.ptr()] {
        auto* window = Application::the().key_window();
        // The desktop itself is no window to close.
        if (window && window != desktop)
            window->close();
    });
    file.add_separator();
    file.add_with_shortcut("Move to Trash"_sv, KeyBackspace, "\xE2\x8C\x98\xE2\x8C\xAB"_sv, [selected_view] {
        if (auto* view = selected_view())
            view->move_selection_to_trash();
    });
    auto& file_item = menus->items().insert_returning(1, "File"_sv);
    file_item.submenu = adopt_own(file);
    auto& go = *new Menu("Go"_sv);
    auto go_to = [&controller](String path) {
        return [&controller, path = move(path)] {
            if (auto* window = controller.key_window())
                window->navigate_to(path.view());
            else
                controller.open_new_window(path.view());
        };
    };
    go.add("Home"_sv, go_to(MUST(String::from_view(home.view()))));
    go.add("Desktop"_sv, go_to(joined(home.view(), "Desktop"_sv)));
    go.add("Documents"_sv, go_to(joined(home.view(), "Documents"_sv)));
    go.add("Downloads"_sv, go_to(joined(home.view(), "Downloads"_sv)));
    go.add("Applications"_sv, go_to(MUST(String::from_view("/Applications"_sv))));
    auto& go_item = menus->items().insert_returning(3, "Go"_sv);
    go_item.submenu = adopt_own(go);
    application->set_main_menu(move(menus));

    printf("files: ready\n");
    fflush(stdout);
    return application->exec();
}
