/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include "Operations.h"
#include "Views.h"
#include <Tags.h>
#include <Desktop.h>
#include <LibDiskManagement/Disks.h>
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
#include <ContentTypes.h>
#include <LibBase/HashMap.h>
#include <QuickView.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <notify.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Files: folders as windows, and the desktop as a folder under them all. There is one
// of it, which others reach at com.vulture.Files to show them a folder.

using namespace UI;
using namespace Gfx;
using namespace FilesApp;

namespace {

constexpr StringView s_file_type = "public.file-url"_sv;

Appearance const& tokens()
{
    return Appearance::the();
}

class PreviewView;

// Quick View: one window, for whatever's selected.
class QuickViewPanel {
public:
    static QuickViewPanel& the()
    {
        static QuickViewPanel panel;
        return panel;
    }

    [[nodiscard]] bool is_open() const { return m_window && m_open; }
    void show(StringView path);
    void close();

    Function<bool(KeyEvent const&)> on_key;

private:
    void ensure_window();
    void picture_arrived(RefPtr<Gfx::Bitmap>);

    OwnPtr<UI::Window> m_window;
    PreviewView* m_view { nullptr };
    Toolbar* m_toolbar { nullptr };
    Button* m_open_button { nullptr };
    String m_path;
    u32 m_generation { 0 };
    bool m_open { false };
};

class PreviewView final : public View {
public:
    void set_contents(RefPtr<Gfx::Bitmap> bitmap, StringView icon, StringView name, StringView description, StringView size)
    {
        m_bitmap = move(bitmap);
        m_icon = MUST(String::from_view(icon));
        m_name = MUST(String::from_view(name));
        m_description = MUST(String::from_view(description));
        m_size = MUST(String::from_view(size));
        set_needs_display();
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        graphics.fill({ 0, 0, width(), height() }, t.color("viewer.background"_sv));
        if (m_bitmap) {
            float const scale = min(1.f, min(width() / bitmap_width(), height() / bitmap_height()));
            float const shown_width = bitmap_width() * scale;
            float const shown_height = bitmap_height() * scale;
            graphics.painter().draw_bitmap(*m_bitmap, graphics.snapped({ (width() - shown_width) / 2, (height() - shown_height) / 2, shown_width, shown_height }));
            return;
        }
        // Nothing can picture it: what it is, instead.
        float const icon_size = 128;
        auto icon = Desktop::icon_named(m_icon.view(), static_cast<int>(icon_size), static_cast<int>(graphics.scale()));
        float const top = (height() - icon_size - 60) / 2;
        if (icon)
            graphics.painter().draw_bitmap(*icon, FloatRect { (width() - icon_size) / 2, top, icon_size, icon_size });
        auto title = t.named_font("title3"_sv);
        auto body = t.named_font("body"_sv);
        graphics.draw_text_centered(m_name.view(), title, { 0, top + icon_size + 8, width(), 24 }, t.color("label"_sv));
        StringBuilder detail;
        detail.appendff("{} \xE2\x80\x94 {}", m_description.view(), m_size.view());
        graphics.draw_text_centered(detail.view(), body, { 0, top + icon_size + 36, width(), 20 }, t.color("secondaryLabel"_sv));
    }

    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    [[nodiscard]] bool shows_focus_ring() const override { return false; }
    bool key_down(KeyEvent const& event) override
    {
        if (event.code == KeySpace || event.code == KeyEscape) {
            QuickViewPanel::the().close();
            return true;
        }
        auto& handler = QuickViewPanel::the().on_key;
        return handler && handler(event);
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Image; }

private:
    [[nodiscard]] float bitmap_width() const { return static_cast<float>(m_bitmap->width()) / 2; }
    [[nodiscard]] float bitmap_height() const { return static_cast<float>(m_bitmap->height()) / 2; }

    RefPtr<Gfx::Bitmap> m_bitmap;
    String m_icon;
    String m_name;
    String m_description;
    String m_size;
};

void QuickViewPanel::ensure_window()
{
    if (m_window)
        return;
    auto screen = Platform::the().screen_size();
    int const width = 760;
    int const height = 560;
    m_window = adopt_own(*new UI::Window("Quick View"_sv, { (screen.width - width) / 2, (screen.height - height) / 3, width, height }, WS::WindowTitled | WS::WindowResizable | WS::WindowUnifiedTitle));
    m_window->backend().set_level(WS::LevelPopup);
    auto& stack = m_window->set_content<StackView>(Axis::Vertical);
    m_toolbar = &stack.add<Toolbar>();
    m_toolbar->add_flexible_space();
    m_open_button = &m_toolbar->add_item<Button>("Open"_sv);
    m_open_button->on_action = [this] {
        auto path = m_path;
        close();
        (void)Desktop::open_path(path.view());
    };
    m_view = &stack.add<PreviewView>();
    m_view->set_accessibility_label("Quick View"_sv);
    m_view->set_stretch(1, 1);
    m_window->on_close_request = [this] {
        close();
        return false;
    };
}

void QuickViewPanel::show(StringView path)
{
    ensure_window();
    m_path = MUST(String::from_view(path));
    auto name = last_component(path);
    m_toolbar->set_title(name);
    m_window->set_title(name);
    StringBuilder open_title;
    if (auto application = Desktop::default_application_for_path(path); application.has_value())
        open_title.appendff("Open with {}", application->name.view());
    else
        open_title.append("Open"_sv);
    m_open_button->set_title(open_title.view());
    if (!m_open) {
        m_window->show();
        m_open = true;
    }
    m_window->make_first_responder(m_view);
    auto const generation = ++m_generation;
    m_view->set_contents(nullptr, Desktop::icon_name_for_path(path).view(), name, Desktop::content_description_of_path(path), {});
    Desktop::request_preview(path, 1520, 1120, [this, generation](RefPtr<Gfx::Bitmap> bitmap) {
        if (generation == m_generation)
            picture_arrived(move(bitmap));
    });
}

void QuickViewPanel::picture_arrived(RefPtr<Gfx::Bitmap> bitmap)
{
    auto name = last_component(m_path.view());
    printf("files: quick view: %s %.*s\n", bitmap ? "picture of" : "no picture of", static_cast<int>(name.length()), name.characters_without_null_termination());
    fflush(stdout);
    if (bitmap) {
        // The window, the shape of what's in it, around the same middle.
        auto const frame = m_window->frame();
        int const toolbar = frame.height - static_cast<int>(m_view->height());
        int const width = max(bitmap->width() / 2, 320);
        int const height = max(bitmap->height() / 2, 200) + toolbar;
        m_window->set_frame({ frame.x + (frame.width - width) / 2, frame.y + (frame.height - height) / 2, width, height });
        m_view->set_contents(move(bitmap), {}, {}, {}, {});
        return;
    }
    struct stat information {};
    (void)stat(m_path.characters(), &information);
    StringBuilder size;
    size.appendff("{} bytes", static_cast<i64>(information.st_size));
    m_view->set_contents(nullptr, Desktop::icon_name_for_path(m_path.view()).view(), name, Desktop::content_description_of_path(m_path.view()), size.view());
}

void QuickViewPanel::close()
{
    if (!m_open)
        return;
    m_open = false;
    ++m_generation;
    m_window->hide();
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
        s_folder_views.append(this);
        Thumbnails::the().on_new = [] { redraw_all(); };
    }

    static void reload_all()
    {
        for (auto* view : s_folder_views)
            view->reload();
    }

    static void redraw_all()
    {
        for (auto* view : s_folder_views)
            view->set_needs_display();
    }

    Function<void(StringView)> on_open_folder;
    Function<void()> on_changed;

    ~FolderView() override
    {
        s_folder_views.remove_all_matching([this](FolderView* other) { return other == this; });
        if (m_quick_view_owner == this)
            m_quick_view_owner = nullptr;
        if (m_watch_timer >= 0)
            Application::the().cancel_timer(m_watch_timer);
    }

    void set_folder(StringView path)
    {
        m_path = MUST(String::from_view(path));
        reload();
        // What's put in it by anyone else (a screenshot on the desktop, a download) shows up: as its folder changes.
        if (m_watch_timer < 0)
            m_watch_timer = Application::the().add_timer(500, true, [this] {
                if (folder_stamp() != m_stamp || (m_on_desktop && external_disks_changed()))
                    reload();
            });
    }

    [[nodiscard]] StringView path() const { return m_path.view(); }
    [[nodiscard]] Vector<FileItem> const& items() const { return m_items; }

    // On the desktop: disks, as the settings say, and what's here gathered into Stacks if that's on.
    void add_desktop_extras()
    {
        Desktop::Preferences preferences("com.vulture.Files"_sv);
        Vector<FileItem> disks;
        if (preferences.boolean("showHardDisks"_sv).value_or(false)) {
            FileItem disk;
            disk.name = MUST(String::from_view("Vulture HD"_sv));
            disk.path = MUST(String::from_view("/"_sv));
            disk.icon = MUST(String::from_view("Drive"_sv));
            disk.is_directory = true;
            disk.is_volume = true;
            disks.append(move(disk));
        }
        if (preferences.boolean("showExternalDisks"_sv).value_or(true)) {
            if (DIR* directory = opendir("/Volumes")) {
                while (auto* entry = readdir(directory)) {
                    StringView const name { entry->d_name, strlen(entry->d_name) };
                    if (name == "."_sv || name == ".."_sv || name == "EFI"_sv || name.starts_with('.'))
                        continue;
                    FileItem disk;
                    disk.name = MUST(String::from_view(name));
                    disk.path = joined("/Volumes"_sv, name);
                    disk.icon = MUST(String::from_view("Drive"_sv));
                    disk.is_directory = true;
                    disk.is_volume = true;
                    disks.append(move(disk));
                }
                closedir(directory);
            }
        }
        m_volume_count = disks.size();
        Vector<FileItem> shown;
        for (auto& disk : disks)
            shown.append(move(disk));
        if (!preferences.boolean("useStacks"_sv).value_or(false)) {
            for (auto& item : m_items)
                shown.append(move(item));
            m_items = move(shown);
            return;
        }
        // Folders stay as they are; what's left, by kind.
        for (auto& item : m_items) {
            if (item.is_directory)
                shown.append(move(item));
        }
        StringView const kinds[] = { "Documents"_sv, "Images"_sv, "Screenshots"_sv, "Movies"_sv, "Music"_sv, "Other"_sv };
        for (auto kind : kinds) {
            int count = 0;
            for (auto const& item : m_items) {
                if (!item.name.is_empty() && !item.is_directory && kind_of_item(item) == kind)
                    ++count;
            }
            if (count == 0)
                continue;
            FileItem stack;
            stack.name = MUST(String::from_view(kind));
            StringBuilder path;
            path.appendff("stack:{}", kind);
            stack.path = MUST(path.to_string());
            stack.icon = MUST(String::from_view("GenericDocument"_sv));
            stack.is_stack = true;
            stack.stack_count = count;
            for (auto const& item : m_items) {
                if (item.name.is_empty() || item.is_directory || kind_of_item(item) != kind)
                    continue;
                if (stack.stack_tops.is_empty())
                    stack.icon = item.icon;
                stack.stack_tops.append(StackTop { item.path, item.icon, item.modified, item.has_thumbnail });
                if (stack.stack_tops.size() == 3)
                    break;
            }
            shown.append(move(stack));
            if (m_open_stack.view() != kind)
                continue;
            for (auto& item : m_items) {
                if (!item.name.is_empty() && !item.is_directory && kind_of_item(item) == kind)
                    shown.append(move(item));
            }
        }
        m_items = move(shown);
    }

    [[nodiscard]] static StringView kind_of_item(FileItem const& item)
    {
        auto type = Desktop::content_type_of_path(item.path.view());
        if (Desktop::content_type_conforms(type, "public.image"_sv))
            return item.name.view().starts_with("Screenshot"_sv) ? "Screenshots"_sv : "Images"_sv;
        if (Desktop::content_type_conforms(type, "public.text"_sv) || Desktop::content_type_conforms(type, "com.adobe.pdf"_sv))
            return "Documents"_sv;
        if (Desktop::content_type_conforms(type, "public.movie"_sv))
            return "Movies"_sv;
        if (Desktop::content_type_conforms(type, "public.audio"_sv))
            return "Music"_sv;
        return "Other"_sv;
    }

    void reload()
    {
        // What was chosen still is, if it's still there: something else changing in the folder isn't a reason to lose it.
        Vector<String> were_selected;
        for (auto const& item : m_items) {
            if (item.selected)
                were_selected.append(item.path);
        }
        m_stamp = folder_stamp();
        m_items = list_folder(m_path.view(), m_path.view().ends_with("/.Trash"_sv));
        if (m_on_desktop)
            add_desktop_extras();
        for (auto& item : m_items) {
            for (auto const& path : were_selected) {
                if (item.path.view() == path.view())
                    item.selected = true;
            }
        }
        if (parent())
            parent()->set_needs_layout();
        set_needs_layout();
        set_needs_display();
        if (on_changed)
            on_changed();
    }

    [[nodiscard]] Vector<String> selected_paths() const
    {
        if (m_external_selection_on)
            return m_external_selection;
        Vector<String> paths;
        for (auto const& item : m_items) {
            if (item.selected && !item.is_stack)
                paths.append(MUST(String::from_view(item.path.view())));
        }
        return paths;
    }

    // Another way of showing this folder has its own idea of what's selected: it's what the menus act on.
    void set_external_selection(bool on, Vector<String> paths = {})
    {
        m_external_selection_on = on;
        m_external_selection = move(paths);
    }

    [[nodiscard]] Vector<FileItem>& mutable_items() { return m_items; }

    void show_info_for_selection()
    {
        for (auto const& path : selected_paths())
            show_get_info(path.view(), [] { reload_all(); });
    }

    void duplicate_selection()
    {
        for (auto const& path : selected_paths())
            (void)duplicate(path.view());
        reload();
    }

    void alias_selection()
    {
        for (auto const& path : selected_paths())
            (void)make_alias(path.view());
        reload();
    }

    void compress_selection()
    {
        auto paths = selected_paths();
        compress(move(paths), [this] {
            printf("files: compressed\n");
            fflush(stdout);
            reload();
        });
    }

    void put_back_selection()
    {
        for (auto const& path : selected_paths())
            (void)put_back(path.view());
        reload();
    }

    void toggle_tag_on_selection(StringView tag)
    {
        for (auto const& path : selected_paths())
            (void)Desktop::toggle_tag(path.view(), tag);
        reload();
    }

    void quick_look_selection() { toggle_quick_view(); }

    [[nodiscard]] bool is_in_trash() const { return m_path.view().ends_with("/.Trash"_sv); }

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
        auto paths = selected_paths();
        move_to_trash(paths.span());
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
        if (s_copied_paths.is_empty() || !window())
            return;
        auto sources = s_copied_paths;
        start_transfer(TransferKind::Copy, move(sources), m_path.view(), *window(), [this] { reload(); });
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
            if (item.is_stack && !item.stack_tops.is_empty()) {
                // A pile: what's in it, each a little further back and up than the one in front, the first on top (and the
                // same one again, if there aren't three).
                for (int depth = 2; depth >= 0; --depth) {
                    auto const& top = item.stack_tops[static_cast<size_t>(depth) % item.stack_tops.size()];
                    float const side = icon_size - 10 - static_cast<float>(depth) * 6;
                    FloatRect const box { icon_x + (icon_size - side) / 2 + static_cast<float>(depth) * 3, rect.y + icon_size - side - static_cast<float>(depth) * 5, side, side };
                    FileItem shown_item;
                    shown_item.path = top.path;
                    shown_item.modified = top.modified;
                    Gfx::Bitmap const* picture = top.has_thumbnail ? Thumbnails::the().get(shown_item, static_cast<int>(icon_size) * scale) : nullptr;
                    if (picture) {
                        float const fit = min(side / static_cast<float>(picture->width()), side / static_cast<float>(picture->height()));
                        float const shown_width = static_cast<float>(picture->width()) * fit;
                        float const shown_height = static_cast<float>(picture->height()) * fit;
                        FloatRect const shape = graphics.snapped({ box.x + (side - shown_width) / 2, box.bottom() - shown_height, shown_width, shown_height });
                        graphics.painter().draw_bitmap(*picture, shape);
                        graphics.stroke_rounded(shape, 0, graphics.pixel(), t.color("separator"_sv));
                    } else if (auto icon = Desktop::icon_named(top.icon.view(), static_cast<int>(icon_size), scale)) {
                        graphics.painter().draw_bitmap(*icon, box);
                    }
                }
            }
            Gfx::Bitmap const* thumbnail = item.has_thumbnail ? Thumbnails::the().get(item, static_cast<int>(icon_size) * scale) : nullptr;
            if (thumbnail) {
                float const fit = min(icon_size / static_cast<float>(thumbnail->width()), icon_size / static_cast<float>(thumbnail->height()));
                float const shown_width = static_cast<float>(thumbnail->width()) * fit;
                float const shown_height = static_cast<float>(thumbnail->height()) * fit;
                FloatRect const shape = graphics.snapped({ icon_x + (icon_size - shown_width) / 2, rect.y + icon_size - shown_height, shown_width, shown_height });
                graphics.painter().draw_bitmap(*thumbnail, shape);
                graphics.stroke_rounded(shape, 0, graphics.pixel(), t.color("separator"_sv));
            } else if (!item.is_stack) {
                if (auto icon = Desktop::icon_named(item.icon.view(), static_cast<int>(icon_size), scale))
                    graphics.painter().draw_bitmap(*icon, FloatRect { icon_x, rect.y, icon_size, icon_size });
            }
            if (static_cast<int>(i) == m_spring_index)
                graphics.fill_rounded({ icon_x - 4, rect.y - 3, icon_size + 8, icon_size + 6 }, 5, t.color("files.selection"_sv));
            if (item.is_alias) {
                // The arrow in the corner that says this is only a way to something else.
                FloatRect const badge { icon_x + 1, rect.y + icon_size - 19, 18, 18 };
                graphics.fill_rounded(badge, 4, Color { 255, 255, 255, 235 });
                graphics.line({ badge.x + 5, badge.bottom() - 5 }, { badge.right() - 5, badge.y + 5 }, 2, Color { 30, 30, 30, 255 }, true);
                graphics.line({ badge.x + 8, badge.y + 5 }, { badge.right() - 5, badge.y + 5 }, 2, Color { 30, 30, 30, 255 }, true);
                graphics.line({ badge.right() - 5, badge.y + 5 }, { badge.right() - 5, badge.bottom() - 8 }, 2, Color { 30, 30, 30, 255 }, true);
            }
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
            float dot_x = label.right() + 2;
            for (size_t tag = item.tags.size(); tag > 0; --tag) {
                auto color = Desktop::color_of_tag(item.tags[tag - 1].view());
                if (!color.has_value())
                    continue;
                graphics.fill_circle({ dot_x + 5, label.y + label.height / 2 }, 5, color.value());
                dot_x -= 6;
            }
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
            // On the desktop, that's the wallpaper.
            if (m_on_desktop && !event.modifiers)
                (void)Application::the().connection().server().wallpaper_clicked();
            return true;
        }
        if (m_items[static_cast<size_t>(hit)].is_stack) {
            auto name = MUST(String::from_view(m_items[static_cast<size_t>(hit)].name.view()));
            m_open_stack = m_open_stack.view() == name.view() ? String {} : move(name);
            reload();
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

    // The Edit menu's, which has Cmd-C, Cmd-V and Cmd-A before a view sees them.
    bool perform_command(StringView command) override
    {
        if (command == "copy"_sv) {
            copy_selection();
            return true;
        }
        if (command == "paste"_sv) {
            paste();
            return true;
        }
        if (command == "selectAll"_sv) {
            for (auto& item : m_items)
                item.selected = true;
            set_needs_display();
            if (on_changed)
                on_changed();
            return true;
        }
        return false;
    }
    [[nodiscard]] bool can_perform_command(StringView command) const override
    {
        if (command == "copy"_sv)
            return !selected_paths().is_empty();
        if (command == "paste"_sv)
            return !s_copied_paths.is_empty();
        return command == "selectAll"_sv;
    }

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
            case KeyI:
                show_info_for_selection();
                return true;
            case KeyD:
                duplicate_selection();
                return true;
            case KeyY:
                toggle_quick_view();
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
        if (event.code == KeySpace) {
            toggle_quick_view();
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
        cancel_spring();
        set_needs_display();
    }

    void drag_moved(FloatPoint point) override
    {
        int const hit = item_at(point);
        bool const on_folder = hit >= 0 && m_items[static_cast<size_t>(hit)].is_directory && !m_items[static_cast<size_t>(hit)].name.view().ends_with(".app"_sv);
        int const wanted = on_folder ? hit : -1;
        if (wanted == m_spring_index)
            return;
        cancel_spring();
        m_spring_index = wanted;
        set_needs_display();
        if (wanted < 0)
            return;
        // Held over a folder for a moment, it opens, and the drag goes on inside it.
        m_spring_timer = Application::the().add_timer(s_spring_milliseconds, false, [this] {
            m_spring_timer = -1;
            if (m_spring_index < 0 || static_cast<size_t>(m_spring_index) >= m_items.size())
                return;
            auto path = m_items[static_cast<size_t>(m_spring_index)].path;
            m_spring_index = -1;
            printf("files: sprang open %s\n", last_component(path.view()).characters_without_null_termination());
            fflush(stdout);
            if (on_open_folder)
                on_open_folder(path.view());
        });
    }

    void perform_drop(StringView, ReadonlyBytes data) override
    {
        m_drop_target = false;
        auto const spring = m_spring_index;
        cancel_spring();
        auto from = MUST(String::from_view(StringView { reinterpret_cast<char const*>(data.data()), data.size() }));
        auto folder = MUST(String::from_view(m_path.view()));
        if (spring >= 0 && static_cast<size_t>(spring) < m_items.size() && m_items[static_cast<size_t>(spring)].path.view() != from.view())
            folder = m_items[static_cast<size_t>(spring)].path;
        if (is_smart_path(folder.view()) || parent_of(from.view()).view() == folder.view() || !window())
            return;
        // Between disks, it's a copy: a Mac leaves the original where it was.
        struct stat a {};
        struct stat b {};
        bool const same_disk = stat(from.characters(), &a) == 0 && stat(folder.characters(), &b) == 0 && a.st_dev == b.st_dev;
        Vector<String> sources;
        sources.append(from);
        start_transfer(same_disk ? TransferKind::Move : TransferKind::Copy, move(sources), folder.view(), *window(), [this] { reload(); });
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Table; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(last_component(m_path.view()))); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        for (size_t i = 0; i < m_items.size(); ++i)
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Cell, m_items[i].name.view(), item_rect(i), m_items[i].selected ? "selected"_sv : StringView {}));
    }

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
        if (path.ends_with(saved_search_extension)) {
            if (auto smart = smart_path_from_file(path); smart.has_value() && on_open_folder)
                on_open_folder(smart->view());
            return;
        }
        // An archive opens as what's in it, beside it.
        if (path.ends_with(".zip"_sv)) {
            expand(path, [this] {
                printf("files: expanded\n");
                fflush(stdout);
                reload();
            });
            return;
        }
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

    void erase_selection()
    {
        auto paths = selected_paths();
        for (auto const& path : paths)
            (void)remove_tree(path.view());
        reload();
    }

public:
    void toggle_stacks()
    {
        Desktop::Preferences preferences("com.vulture.Files"_sv);
        preferences.set("useStacks"_sv, JSON::Value(!preferences.boolean("useStacks"_sv).value_or(false)));
        preferences.save();
        m_open_stack = {};
        reload();
    }

private:
    void toggle_quick_view()
    {
        auto& panel = QuickViewPanel::the();
        if (panel.is_open()) {
            panel.close();
            return;
        }
        auto const paths = selected_paths();
        if (paths.is_empty())
            return;
        m_quick_view_owner = this;
        // Arrow keys go on moving through what's here, with the panel following.
        panel.on_key = [](KeyEvent const& event) {
            auto* owner = m_quick_view_owner;
            if (!owner || !owner->key_down(event))
                return false;
            auto const moved = owner->selected_paths();
            if (!moved.is_empty())
                QuickViewPanel::the().show(moved[0].view());
            return true;
        };
        panel.show(paths[0].view());
    }

    void add_open_with(Menu& menu, StringView path)
    {
        auto applications = Desktop::applications_for_path(path);
        auto& open_with = menu.add_submenu("Open With"_sv);
        if (applications.is_empty()) {
            open_with.add("No Applications"_sv).enabled = false;
            return;
        }
        auto file = MUST(String::from_view(path));
        auto content_type = MUST(String::from_view(Desktop::content_type_of_path(path)));
        for (size_t i = 0; i < applications.size(); ++i) {
            StringBuilder title;
            title.append(applications[i].name.view());
            if (i == 0)
                title.append(" (default)"_sv);
            auto application = applications[i];
            auto& item = open_with.add(title.view(), [file, application] {
                StringView arguments[1] = { file.view() };
                (void)Desktop::launch(application, Span<StringView const>(arguments, 1));
            });
            // With Option held, it's from now on, as Finder's Always Open With is.
            StringBuilder always;
            always.appendff("Always Open With {}", applications[i].name.view());
            auto identifier = applications[i].identifier;
            Menu::add_alternate(item, always.view(), [file, application, content_type, identifier] {
                Desktop::set_default_application(content_type.view(), identifier.view());
                StringView arguments[1] = { file.view() };
                (void)Desktop::launch(application, Span<StringView const>(arguments, 1));
            });
        }
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
            if (item.is_stack)
                return;
            if (!item.is_directory)
                add_open_with(*m_menu, item.path.view());
            m_menu->add_separator();
            if (is_in_trash()) {
                auto& back = m_menu->add("Put Back"_sv, [this] { put_back_selection(); });
                back.enabled = can_put_back(item.path.view());
                m_menu->add("Delete Immediately\xE2\x80\xA6"_sv, [this] { erase_selection(); });
                m_menu->add_separator();
                m_menu->add("Empty Trash"_sv, [this] {
                    empty_trash(*window(), [this] { reload(); });
                });
            } else if (item.is_volume) {
                m_menu->add("Eject"_sv, [path = item.path] {
                    StringBuilder line;
                    line.appendff("eject {}", path.view());
                    (void)DiskManagement::request(line.view());
                });
            } else {
                m_menu->add("Move to Trash"_sv, [this] { move_selection_to_trash(); });
            }
            m_menu->add_separator();
            m_menu->add("Get Info"_sv, [this] { show_info_for_selection(); });
            m_menu->add("Rename"_sv, [this] { begin_renaming(); });
            auto selected_count = selected_paths().size();
            StringBuilder compress_title;
            if (selected_count == 1)
                compress_title.appendff("Compress \xE2\x80\x9C{}\xE2\x80\x9D", item.name.view());
            else
                compress_title.appendff("Compress {} Items", selected_count);
            m_menu->add(compress_title.view(), [this] { compress_selection(); });
            m_menu->add("Duplicate"_sv, [this] { duplicate_selection(); });
            m_menu->add("Make Alias"_sv, [this] { alias_selection(); });
            StringBuilder look;
            look.appendff("Quick Look \xE2\x80\x9C{}\xE2\x80\x9D", item.name.view());
            m_menu->add(look.view(), [this] { toggle_quick_view(); });
            m_menu->add_separator();
            m_menu->add("Copy"_sv, [this] { copy_selection(); });
            auto& tags_menu = m_menu->add_submenu("Tags"_sv);
            for (auto const& tag : Desktop::standard_tags()) {
                auto name = MUST(String::from_view(tag.name));
                auto& entry = tags_menu.add(tag.name, [this, name] { toggle_tag_on_selection(name.view()); });
                for (auto const& existing : item.tags)
                    entry.checked = entry.checked || existing.view() == tag.name;
            }
            if (Desktop::content_type_of_path(item.path.view()) == "public.png"_sv) {
                auto& actions = m_menu->add_submenu("Quick Actions"_sv);
                actions.add("Rotate Left"_sv, [this, path = item.path] { rotate_picture(path.view(), false, [this] { reload(); }); });
                actions.add("Rotate Right"_sv, [this, path = item.path] { rotate_picture(path.view(), true, [this] { reload(); }); });
            }
        } else {
            m_menu->add("New Folder"_sv, [this] { new_folder(); });
            auto& paste_item = m_menu->add("Paste"_sv, [this] { paste(); });
            paste_item.enabled = !s_copied_paths.is_empty();
            // The desktop's own, as Sonoma's Finder has them: its wallpaper, and its widgets (the shell's).
            if (m_on_desktop) {
                m_menu->add_separator();
                Desktop::Preferences preferences("com.vulture.Files"_sv);
                auto& stacks = m_menu->add("Use Stacks"_sv, [this] { toggle_stacks(); });
                stacks.checked = preferences.boolean("useStacks"_sv).value_or(false);
                m_menu->add("Change Wallpaper\xE2\x80\xA6"_sv, [] {
                    auto bundle = Desktop::read_bundle("/System/Applications/Settings.app"_sv);
                    StringView arguments[1] = { "Wallpaper"_sv };
                    if (!bundle.is_error())
                        (void)Desktop::launch(bundle.value(), Span<StringView const>(arguments, 1));
                });
                m_menu->add("Edit Widgets\xE2\x80\xA6"_sv, [] { notify_post("com.vulture.widgets.edit"); });
            }
        }
        auto origin = window()->frame().origin();
        m_menu->open(*window(), { origin.x + static_cast<int>(event.window_position.x), origin.y + static_cast<int>(event.window_position.y) });
    }

    [[nodiscard]] bool external_disks_changed() const
    {
        size_t count = 0;
        if (DIR* directory = opendir("/Volumes")) {
            while (auto* entry = readdir(directory)) {
                StringView const name { entry->d_name, strlen(entry->d_name) };
                if (name != "."_sv && name != ".."_sv && name != "EFI"_sv && !name.starts_with('.'))
                    ++count;
            }
            closedir(directory);
        }
        return count != m_seen_external_disks ? (m_seen_external_disks = count, true) : false;
    }

    mutable size_t m_seen_external_disks { 0 };

    void cancel_spring()
    {
        if (m_spring_timer >= 0)
            Application::the().cancel_timer(m_spring_timer);
        m_spring_timer = -1;
        m_spring_index = -1;
    }

    static constexpr u64 s_spring_milliseconds = 700;
    int m_spring_index { -1 };
    int m_spring_timer { -1 };
    size_t m_volume_count { 0 };
    String m_open_stack;
    bool m_external_selection_on { false };
    Vector<String> m_external_selection;
    static inline Vector<FolderView*> s_folder_views;
    static inline FolderView* m_quick_view_owner { nullptr };

    bool m_on_desktop;
    // When the folder last changed, as far as it's been seen: when its entries did.
    [[nodiscard]] u64 folder_stamp() const
    {
        struct stat information {};
        if (stat(m_path.characters(), &information) != 0)
            return 0;
        return static_cast<u64>(information.st_mtime) * 1'000'000'000ull + static_cast<u64>(information.st_mtim.tv_nsec) + static_cast<u64>(information.st_size);
    }

    String m_path;
    u64 m_stamp { 0 };
    int m_watch_timer { -1 };
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
            builder.append(date_text(item.modified).view());
            break;
        case 2:
            builder.append(item.is_directory ? "--"_sv : size_text(item.size).view());
            break;
        default:
            builder.append(Desktop::content_description_of_path(item.path.view()));
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
    void open_new_tab(StringView path);
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

    // What the Edit menu asks of whatever has the keyboard, if that's the sidebar or a list: it's the folder's to do.
    FolderView* folder { nullptr };
    bool perform_command(StringView command) override { return folder && folder->perform_command(command); }
    [[nodiscard]] bool can_perform_command(StringView command) const override { return folder && folder->can_perform_command(command); }
};

class FilesWindow {
public:
    ~FilesWindow()
    {
        // Its timer would go on calling a window that's gone.
        if (m_volumes_timer >= 0)
            Application::the().cancel_timer(m_volumes_timer);
    }

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
        m_view_mode->add_segment(Symbol::ViewIcons, "Icons"_sv);
        m_view_mode->add_segment(Symbol::ViewList, "List"_sv);
        m_view_mode->add_segment(Symbol::ViewColumns, "Columns"_sv);
        m_view_mode->add_segment(Symbol::ViewGallery, "Gallery"_sv);
        m_view_mode->set_selected(0);
        m_view_mode->on_change = [this](size_t) { update_view_mode(); };
        (void)m_toolbar->add_button(Symbol::Folder, [this] { m_icons->new_folder(); }, "New Folder"_sv);
        m_save_search = &m_toolbar->add_item<Button>("Save"_sv);
        m_save_search->set_hidden(true);
        m_save_search->on_action = [this] { ask_to_save_search(); };
        m_empty_trash = &m_toolbar->add_item<Button>("Empty"_sv);
        m_empty_trash->set_hidden(true);
        m_empty_trash->on_action = [this] { empty_trash(*m_window, [this] { m_icons->reload(); }); };
        m_search = &m_toolbar->add_search(150);
        m_search->on_change = [this] { search_changed(); };
        auto& body = right.add<StackView>(Axis::Horizontal);
        body.set_stretch(0, 1);
        auto& center = body.add<StackView>(Axis::Vertical);
        center.set_stretch(1, 1);
        m_preview = &body.add<PreviewPane>();
        m_preview->set_fixed_width(260);
        m_preview->set_hidden(true);
        m_preview->on_changed = [this] { m_icons->reload(); };
        m_scroll = &center.add<ScrollView>();
        m_scroll->set_stretch(0, 1);
        m_scroll->set_scrolls_horizontally(false);
        m_icons = &m_scroll->make_document<FolderView>(false);
        root.folder = m_icons;
        m_icons->on_open_folder = [this](StringView folder) { navigate(folder, true); };
        m_icons->on_changed = [this] {
            selection_changed();
            m_table->reload();
            sync_list_selection();
            if (mode() == ViewMode::Gallery)
                m_gallery->refresh_selection();
            if (mode() == ViewMode::Columns)
                m_columns->refresh();
        };
        m_table = &center.add<TableView>(&m_list_model);
        m_table->add_column("Name"_sv, 260);
        m_table->add_column("Date Modified"_sv, 180);
        m_table->add_column("Size"_sv, 70);
        m_table->add_column("Kind"_sv, 130);
        m_table->set_stretch(0, 1);
        m_table->set_hidden(true);
        m_table->on_activate = [this](size_t row) { open_list_row(row); };
        m_table->on_selection_change = [this] { list_selection_changed(); };
        m_columns = &center.add<ColumnBrowser>();
        m_columns->set_stretch(0, 1);
        m_columns->set_hidden(true);
        m_columns->on_open = [this](StringView path) { open_from_view(path); };
        m_columns->on_selection_change = [this] {
            m_icons->set_external_selection(true, m_columns->selected_paths());
            update_status();
        };
        m_columns->on_quick_view = [this] { m_icons->quick_look_selection(); };
        m_gallery = &center.add<GalleryView>();
        m_gallery->set_stretch(0, 1);
        m_gallery->set_hidden(true);
        m_gallery->on_open = [this](StringView path) { open_from_view(path); };
        m_gallery->on_selection_change = [this] { update_status(); };
        m_gallery->on_quick_view = [this] { m_icons->quick_look_selection(); };
        m_path_bar = &right.add<PathBar>();
        m_path_bar->set_fixed_height(24);
        m_path_bar->set_hidden(true);
        m_path_bar->on_navigate = [this](StringView path) {
            auto owned = MUST(String::from_view(path));
            navigate(owned.view(), true);
        };
        m_status = &right.add<Label>(StringView {}, t.named_font("small"_sv));
        m_status->set_style(LabelStyle::Secondary);
        m_status->set_alignment(TextAlignment::Center);
        m_status->set_fixed_height(22);

        auto home = Desktop::home_directory();
        m_sidebar->add_section("Favorites"_sv);
        add_place("Recents"_sv, recents_path().view(), Symbol::Clock);
        add_place("Home"_sv, home.view(), Symbol::Home);
        add_place("Desktop"_sv, joined(home.view(), "Desktop"_sv).view(), Symbol::Desktop);
        add_place("Documents"_sv, joined(home.view(), "Documents"_sv).view(), Symbol::File);
        add_place("Downloads"_sv, joined(home.view(), "Downloads"_sv).view(), Symbol::Download);
        add_place("Applications"_sv, "/Applications"_sv, Symbol::Applications);
        m_sidebar->add_section("Locations"_sv);
        m_system_row = m_places.size();
        add_place("Vulture HD"_sv, "/"_sv, Symbol::Drive);
        add_place("Trash"_sv, joined(home.view(), ".Trash"_sv).view(), Symbol::Trash);
        add_smart_folders();
        m_sidebar->add_section("Tags"_sv);
        for (auto const& tag : Desktop::standard_tags()) {
            add_place(tag.name, tag_path(tag.name).view(), Symbol::TagDot);
            m_sidebar->set_row_tint(m_places.size() - 1, tag.color);
        }
        refresh_volumes();
        m_volumes_timer = Application::the().add_timer(500, true, [this] { refresh_volumes(); });
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
    [[nodiscard]] StringView current_path() const { return m_icons->path(); }

    void set_mode(size_t index)
    {
        m_view_mode->set_selected(index);
        update_view_mode();
    }

    void toggle_path_bar()
    {
        m_path_bar->set_hidden(!m_path_bar->is_hidden());
        printf("files: path bar %s\n", m_path_bar->is_hidden() ? "hidden" : "shown");
        fflush(stdout);
    }

    void toggle_status_bar() { m_status->set_hidden(!m_status->is_hidden()); }

    void toggle_preview()
    {
        m_preview->set_hidden(!m_preview->is_hidden());
        update_preview();
        printf("files: preview %s\n", m_preview->is_hidden() ? "hidden" : "shown");
        fflush(stdout);
    }

    void focus_search() { (void)m_window->make_first_responder(m_search); }

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
    enum class ViewMode : u8 {
        Icons,
        List,
        Columns,
        Gallery,
    };

    [[nodiscard]] ViewMode mode() const { return static_cast<ViewMode>(max(m_view_mode->selected_index(), 0)); }

    void open_from_view(StringView given)
    {
        auto owned = MUST(String::from_view(given));
        auto path = owned.view();
        struct stat information {};
        if (stat(owned.characters(), &information) == 0 && S_ISDIR(information.st_mode) && !path.ends_with(".app"_sv)) {
            navigate(path, true);
            return;
        }
        (void)Desktop::open_path(path);
    }

    void selection_changed()
    {
        update_status();
        update_preview();
    }

    void update_preview()
    {
        if (m_preview->is_hidden())
            return;
        auto const paths = m_icons->selected_paths();
        m_preview->set_path(paths.is_empty() ? StringView {} : paths[0].view());
    }

    void sync_list_selection()
    {
        if (m_syncing_list)
            return;
        m_syncing_list = true;
        m_table->clear_selection();
        auto const& items = m_icons->items();
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].selected)
                m_table->select_row(i, true);
        }
        m_syncing_list = false;
    }

    void list_selection_changed()
    {
        if (m_syncing_list)
            return;
        m_syncing_list = true;
        auto& items = m_icons->mutable_items();
        int const selected = m_table->selected_row();
        for (size_t i = 0; i < items.size(); ++i)
            items[i].selected = static_cast<int>(i) == selected;
        selection_changed();
        m_syncing_list = false;
    }

    void search_changed()
    {
        auto text = MUST(String::from_view(m_search->text()));
        if (text.is_empty()) {
            m_save_search->set_hidden(true);
            if (is_smart_path(m_icons->path()) && !m_before_search.is_empty()) {
                auto back = move(m_before_search);
                m_before_search = {};
                navigate(back.view(), false);
            }
            return;
        }
        if (!is_smart_path(m_icons->path()) || m_before_search.is_empty())
            m_before_search = MUST(String::from_view(m_icons->path()));
        StringBuilder smart;
        smart.appendff("smart:Searching \xE2\x80\x9C{}\xE2\x80\x9D|{}", text.view(), text.view());
        auto path = MUST(smart.to_string());
        m_keep_search_text = true;
        navigate(path.view(), false);
        m_keep_search_text = false;
        m_save_search->set_hidden(false);
    }

    void ask_to_save_search()
    {
        auto const& t = tokens();
        auto root = adopt_own(*new StackView(Axis::Vertical, 10, Alignment::Fill, Insets::all(t.metric("alert.padding"_sv))));
        (void)root->add<Label>("Save this search as a Smart Folder:"_sv);
        auto& field = root->add<TextField>("Smart Folder"_sv);
        field.set_accessibility_label("Name"_sv);
        auto& buttons = root->add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& filler = buttons.add<View>();
        filler.set_stretch(1, 0);
        auto& cancel = buttons.add<Button>("Cancel"_sv);
        cancel.on_action = [this] { Sheet::dismiss(*m_window); };
        auto& save = buttons.add<Button>("Save"_sv, ButtonStyle::Default);
        auto* field_pointer = &field;
        auto const query = MUST(String::from_view(smart_query(m_icons->path())));
        save.on_action = [this, field_pointer, query] {
            auto name = MUST(String::from_view(field_pointer->text()));
            if (name.is_empty())
                return;
            auto saved = save_smart_folder(name.view(), query.view());
            Sheet::dismiss(*m_window);
            if (saved.is_error())
                return;
            printf("files: saved a smart folder\n");
            fflush(stdout);
            if (auto smart = smart_path_from_file(saved.value().view()); smart.has_value())
                add_place(name.view(), smart->view(), Symbol::Magnifier);
        };
        buttons.set_fixed_height(20);
        Sheet::present(*m_window, adopt_own(*static_cast<View*>(root.leak_ptr())), { 340, 130 });
    }

    void add_smart_folders()
    {
        auto folder = joined(Desktop::home_directory().view(), "Library/Saved Searches"_sv);
        for (auto const& item : list_folder(folder.view())) {
            if (!item.name.view().ends_with(saved_search_extension))
                continue;
            auto name = item.name.view().substring_view(0, item.name.length() - saved_search_extension.length());
            if (auto smart = smart_path_from_file(item.path.view()); smart.has_value())
                add_place(name, smart->view(), Symbol::Magnifier);
        }
    }

    void add_place(StringView title, StringView path, Symbol symbol)
    {
        m_sidebar->add_row(title, symbol);
        m_places.append(MUST(String::from_view(path)));
    }

    // What's under /Volumes, after the start-up disk: as they're plugged in and taken out.
    void refresh_volumes()
    {
        // Each by name, and which mount of it: one erased and put back has the same name, and is another volume.
        Vector<String> now;
        Vector<u64> now_ids;
        if (DIR* directory = opendir("/Volumes")) {
            while (auto* entry = readdir(directory)) {
                StringView const name { entry->d_name, strlen(entry->d_name) };
                // (That one's for the machine, and nobody's business.)
                if (name == "."_sv || name == ".."_sv || name == "EFI"_sv || name.starts_with('.'))
                    continue;
                auto const path = joined("/Volumes"_sv, name);
                struct stat root {};
                u64 const id = stat(path.characters(), &root) == 0 ? static_cast<u64>(root.st_dev) : 0;
                now.append(MUST(String::from_view(name)));
                now_ids.append(id);
            }
            closedir(directory);
        }
        for (size_t row = m_shown_volumes.size(); row > 0; --row) {
            bool still_there = false;
            for (size_t i = 0; i < now.size(); ++i)
                still_there = still_there || (now[i].view() == m_shown_volumes[row - 1].view() && now_ids[i] == m_shown_volume_ids[row - 1]);
            if (still_there)
                continue;
            size_t const sidebar_row = m_system_row + row;
            auto const place = MUST(String::from_view(m_places[sidebar_row].view()));
            bool const was_here = m_icons->path().starts_with(place.view());
            m_sidebar->remove_row(sidebar_row);
            m_places.remove(sidebar_row);
            m_shown_volumes.remove(row - 1);
            m_shown_volume_ids.remove(row - 1);
            if (was_here)
                navigate(Desktop::home_directory().view(), false);
        }
        for (size_t index = 0; index < now.size(); ++index) {
            auto const& name = now[index];
            bool shown = false;
            for (auto const& known : m_shown_volumes)
                shown = shown || known.view() == name.view();
            if (shown)
                continue;
            size_t const after = m_system_row + m_shown_volumes.size();
            auto const path = joined("/Volumes"_sv, name.view());
            size_t const row = m_sidebar->insert_row_after(after, name.view(), Symbol::Drive);
            m_places.insert(row, MUST(String::from_view(path.view())));
            m_shown_volumes.append(MUST(String::from_view(name.view())));
            m_shown_volume_ids.append(now_ids[index]);
            StringBuilder label;
            label.appendff("Eject {}", name.view());
            m_sidebar->set_row_accessory(row, Symbol::Eject, label.view(), [path = MUST(String::from_view(path.view()))] {
                StringBuilder line;
                line.appendff("eject {}", path.view());
                (void)DiskManagement::request(line.view());
            });
        }
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
        if (!m_keep_search_text && !m_search->text().is_empty()) {
            m_search->set_text({});
            m_save_search->set_hidden(true);
            m_before_search = {};
        }
        m_icons->set_external_selection(false);
        m_icons->set_folder(path);
        m_list_model.set_items(&m_icons->items());
        m_table->reload();
        if (mode() == ViewMode::Columns)
            m_columns->set_root(path);
        if (mode() == ViewMode::Gallery)
            m_gallery->set_items(&m_icons->mutable_items());
        m_path_bar->set_path(path);
        m_empty_trash->set_hidden(!path.ends_with("/.Trash"_sv));
        StringView shown = path == "/"_sv ? "Vulture HD"_sv : last_component(path);
        if (path.ends_with("/.Trash"_sv))
            shown = "Trash"_sv;
        if (is_smart_path(path))
            shown = smart_name(path);
        m_window->set_title(shown);
        m_title->set_text(shown);
        m_back->set_enabled(!m_back_stack.is_empty());
        m_forward->set_enabled(!m_forward_stack.is_empty());
        // The place it is in the sidebar, or none, as in a folder that isn't one; then any of them can be gone to.
        m_syncing_sidebar = true;
        bool in_sidebar = false;
        for (size_t i = 0; i < m_places.size(); ++i) {
            if (m_places[i].view() == path) {
                m_sidebar->select_row(i);
                in_sidebar = true;
            }
        }
        if (!in_sidebar)
            m_sidebar->clear_selection();
        m_syncing_sidebar = false;
        m_scroll->scroll_to({ 0, 0 });
        update_status();
        update_preview();
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
        auto const shown = mode();
        m_scroll->set_hidden(shown != ViewMode::Icons);
        m_table->set_hidden(shown != ViewMode::List);
        m_columns->set_hidden(shown != ViewMode::Columns);
        m_gallery->set_hidden(shown != ViewMode::Gallery);
        m_icons->set_external_selection(false);
        if (shown == ViewMode::Columns) {
            m_columns->set_root(m_icons->path());
            (void)m_window->make_first_responder(m_columns);
        } else if (shown == ViewMode::Gallery) {
            m_gallery->set_items(&m_icons->mutable_items());
            (void)m_window->make_first_responder(m_gallery);
        } else if (shown == ViewMode::List) {
            sync_list_selection();
        }
        printf("files: shown as %s\n", shown == ViewMode::Icons ? "icons" : shown == ViewMode::List ? "a list" : shown == ViewMode::Columns ? "columns" : "a gallery");
        fflush(stdout);
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
    ColumnBrowser* m_columns { nullptr };
    GalleryView* m_gallery { nullptr };
    PathBar* m_path_bar { nullptr };
    PreviewPane* m_preview { nullptr };
    SearchField* m_search { nullptr };
    Button* m_save_search { nullptr };
    Button* m_empty_trash { nullptr };
    String m_before_search;
    bool m_keep_search_text { false };
    bool m_syncing_list { false };
    ListModel m_list_model;
    Vector<String> m_places;
    size_t m_system_row { 0 };
    Vector<String> m_shown_volumes;
    Vector<u64> m_shown_volume_ids;
    int m_volumes_timer { -1 };
    Vector<String> m_back_stack;
    Vector<String> m_forward_stack;
    bool m_syncing_sidebar { false };
};

void FilesController::open_new_window(StringView path)
{
    int offset = static_cast<int>(m_windows.size()) * 28;
    m_windows.append(adopt_own(*new FilesWindow(*this, path, { 180 + offset, 80 + offset })));
}

void FilesController::open_new_tab(StringView path)
{
    auto* front = key_window();
    if (!front) {
        open_new_window(path);
        return;
    }
    auto frame = front->window().frame();
    auto* window = new FilesWindow(*this, path, { frame.x, frame.y });
    m_windows.append(adopt_own(*window));
    window->window().set_frame(frame);
    front->window().add_tab(window->window());
    printf("files: new tab\n");
    fflush(stdout);
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

// Files > Settings.
void show_settings()
{
    static OwnPtr<UI::Window> s_window;
    if (s_window) {
        s_window->show();
        s_window->backend().activate();
        return;
    }
    auto screen = Platform::the().screen_size();
    s_window = adopt_own(*new UI::Window("Files Settings"_sv, { (screen.width - 340) / 2, (screen.height - 180) / 3, 340, 150 }, WS::WindowTitled));
    auto& stack = s_window->set_content<StackView>(Axis::Vertical, 8, Alignment::Leading, Insets::all(18));
    (void)stack.add<Label>("Show these items on the desktop:"_sv);
    Desktop::Preferences preferences("com.vulture.Files"_sv);
    auto add = [&](StringView title, StringView key, bool fallback) {
        auto& box = stack.add<Checkbox>(title, preferences.boolean(key).value_or(fallback) ? ToggleState::On : ToggleState::Off);
        auto key_string = MUST(String::from_view(key));
        auto* pointer = &box;
        box.on_action = [pointer, key_string] {
            Desktop::Preferences saved("com.vulture.Files"_sv);
            saved.set(key_string.view(), JSON::Value(pointer->state() == ToggleState::On));
            saved.save();
            FolderView::reload_all();
        };
    };
    add("Hard disks"_sv, "showHardDisks"_sv, false);
    add("External disks"_sv, "showExternalDisks"_sv, true);
    s_window->show();
    printf("files: settings shown\n");
    fflush(stdout);
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
    file.add_with_shortcut("New Tab"_sv, KeyT, "\xE2\x8C\x98T"_sv, [&controller, &home] { controller.open_new_tab(home.view()); });
    file.add("New Smart Folder"_sv, [&controller] {
        if (auto* window = controller.key_window())
            window->focus_search();
    });
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
    file.add_with_shortcut("Get Info"_sv, KeyI, "\xE2\x8C\x98I"_sv, [selected_view] {
        if (auto* view = selected_view())
            view->show_info_for_selection();
    });
    file.add("Compress"_sv, [selected_view] {
        if (auto* view = selected_view())
            view->compress_selection();
    });
    file.add_with_shortcut("Duplicate"_sv, KeyD, "\xE2\x8C\x98" "D"_sv, [selected_view] {
        if (auto* view = selected_view())
            view->duplicate_selection();
    });
    file.add("Make Alias"_sv, [selected_view] {
        if (auto* view = selected_view())
            view->alias_selection();
    });
    file.add_with_shortcut("Quick Look"_sv, KeyY, "\xE2\x8C\x98Y"_sv, [selected_view] {
        if (auto* view = selected_view())
            view->quick_look_selection();
    });
    file.add_separator();
    file.add_with_shortcut("Move to Trash"_sv, KeyBackspace, "\xE2\x8C\x98\xE2\x8C\xAB"_sv, [selected_view] {
        if (auto* view = selected_view())
            view->move_selection_to_trash();
    });
    auto& file_item = menus->items().insert_returning(1, "File"_sv);
    file_item.submenu = adopt_own(file);
    auto& view_menu = *new Menu("View"_sv);
    auto with_window = [&controller](auto action) {
        return [&controller, action] {
            if (auto* window = controller.key_window())
                action(*window);
        };
    };
    view_menu.add_with_shortcut("as Icons"_sv, Key1, "\xE2\x8C\x98" "1"_sv, with_window([](FilesWindow& window) { window.set_mode(0); }));
    view_menu.add_with_shortcut("as List"_sv, Key1 + 1, "\xE2\x8C\x98" "2"_sv, with_window([](FilesWindow& window) { window.set_mode(1); }));
    view_menu.add_with_shortcut("as Columns"_sv, Key1 + 2, "\xE2\x8C\x98" "3"_sv, with_window([](FilesWindow& window) { window.set_mode(2); }));
    view_menu.add_with_shortcut("as Gallery"_sv, Key1 + 3, "\xE2\x8C\x98" "4"_sv, with_window([](FilesWindow& window) { window.set_mode(3); }));
    view_menu.add_separator();
    view_menu.add("Use Stacks"_sv, [&desktop] { desktop.toggle_stacks(); });
    view_menu.add_separator();
    view_menu.add("Show Path Bar"_sv, with_window([](FilesWindow& window) { window.toggle_path_bar(); }));
    view_menu.add("Show Status Bar"_sv, with_window([](FilesWindow& window) { window.toggle_status_bar(); }));
    view_menu.add("Show Preview"_sv, with_window([](FilesWindow& window) { window.toggle_preview(); }));
    auto& view_item = menus->items().insert_returning(3, "View"_sv);
    view_item.submenu = adopt_own(view_menu);
    auto& go = *new Menu("Go"_sv);
    auto go_to = [&controller](String path) {
        return [&controller, path = move(path)] {
            if (auto* window = controller.key_window())
                window->navigate_to(path.view());
            else
                controller.open_new_window(path.view());
        };
    };
    go.add("Recents"_sv, go_to(recents_path()));
    go.add("Home"_sv, go_to(MUST(String::from_view(home.view()))));
    go.add("Desktop"_sv, go_to(joined(home.view(), "Desktop"_sv)));
    go.add("Documents"_sv, go_to(joined(home.view(), "Documents"_sv)));
    go.add("Downloads"_sv, go_to(joined(home.view(), "Downloads"_sv)));
    go.add("Applications"_sv, go_to(MUST(String::from_view("/Applications"_sv))));
    auto& go_item = menus->items().insert_returning(4, "Go"_sv);
    go_item.submenu = adopt_own(go);
    // Under its own name: its settings, and the Trash.
    if (auto& own = menus->items()[0]; own.submenu) {
        auto& settings_item = own.submenu->items().insert_returning(2, "Settings\xE2\x80\xA6"_sv);
        settings_item.action = [] { show_settings(); };
        settings_item.shortcut_text = MUST(String::from_view("\xE2\x8C\x98,"_sv));
        auto& empty_item = own.submenu->items().insert_returning(3, "Empty Trash\xE2\x80\xA6"_sv);
        empty_item.action = [&desktop_window, &desktop] { empty_trash(*desktop_window, [&desktop] { desktop.reload(); }); };
    }
    application->set_main_menu(move(menus));

    printf("files: ready\n");
    fflush(stdout);
    return application->exec();
}
