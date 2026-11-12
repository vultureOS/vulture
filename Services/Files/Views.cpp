/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Views.h"
#include "Operations.h"
#include <ContentTypes.h>
#include <Desktop.h>
#include <LibUI/Application.h>
#include <LibUI/Layout.h>
#include <LibUI/Menu.h>
#include <QuickView.h>
#include <Tags.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace UI;
using namespace Gfx;

namespace FilesApp {

namespace {

constexpr int most_thumbnails_at_once = 2;
constexpr float column_width = 240;
constexpr float preview_width = 260;
constexpr float strip_height = 84;
constexpr float strip_cell = 72;

Appearance const& tokens()
{
    return Appearance::the();
}

void draw_tag_dots(Graphics& graphics, Vector<String> const& tags, FloatPoint right_center, float diameter)
{
    float x = right_center.x;
    for (size_t i = tags.size(); i > 0; --i) {
        auto color = Desktop::color_of_tag(tags[i - 1].view());
        if (!color.has_value())
            continue;
        graphics.fill_circle({ x - diameter / 2, right_center.y }, diameter / 2 + 1, tokens().color("textBackground"_sv));
        graphics.fill_circle({ x - diameter / 2, right_center.y }, diameter / 2, color.value());
        x -= diameter * 0.6f;
    }
}

}

// ---- Thumbnails

Thumbnails& Thumbnails::the()
{
    static Thumbnails thumbnails;
    return thumbnails;
}

Bitmap const* Thumbnails::get(FileItem const& item, int pixels)
{
    StringBuilder builder;
    builder.appendff("{}|{}", item.path.view(), item.modified);
    auto key = MUST(builder.to_string());
    if (auto const* found = m_done.get(key.view()))
        return found->ptr();
    if (!m_asked.contains(key.view())) {
        m_asked.set(key, true);
        m_waiting.append(Waiting { key, MUST(String::from_view(item.path.view())), pixels });
        pump();
    }
    return nullptr;
}

void Thumbnails::pump()
{
    while (m_running < most_thumbnails_at_once && !m_waiting.is_empty()) {
        auto next = move(m_waiting.last());
        m_waiting.remove(m_waiting.size() - 1);
        ++m_running;
        auto key = move(next.key);
        Desktop::request_thumbnail(next.path.view(), next.pixels, [this, key = move(key)](RefPtr<Bitmap> bitmap) {
            --m_running;
            m_done.set(key, move(bitmap));
            if (on_new)
                on_new();
            pump();
        });
    }
}

// ---- Tags

void TagPicker::set_path(StringView path)
{
    // (`path` may be a view of m_path, as it is when it looks again after a change: what it says is read before it's replaced.)
    auto copy = MUST(String::from_view(path));
    m_tags = Desktop::tags_of(copy.view());
    m_path = move(copy);
    set_needs_display();
}

FloatSize TagPicker::intrinsic_size() const
{
    return { 7 * 24, 22 };
}

FloatRect TagPicker::dot_rect(size_t index) const
{
    return { static_cast<float>(index) * 24, 2, 18, 18 };
}

void TagPicker::draw(Graphics& graphics)
{
    auto tags = Desktop::standard_tags();
    for (size_t i = 0; i < tags.size(); ++i) {
        auto rect = dot_rect(i);
        graphics.fill_circle({ rect.x + 9, rect.y + 9 }, 8, tags[i].color);
        bool on = false;
        for (auto const& tag : m_tags)
            on = on || tag.view() == tags[i].name;
        if (on)
            graphics.draw_symbol(Symbol::Checkmark, rect.inflated(-3), Color { 255, 255, 255, 255 }, 2);
    }
}

bool TagPicker::mouse_down(MouseEvent const& event)
{
    auto tags = Desktop::standard_tags();
    for (size_t i = 0; i < tags.size(); ++i) {
        if (!dot_rect(i).contains(event.position))
            continue;
        (void)Desktop::toggle_tag(m_path.view(), tags[i].name);
        set_path(m_path.view());
        printf("files: tags of %.*s changed\n", static_cast<int>(last_component(m_path.view()).length()), last_component(m_path.view()).characters_without_null_termination());
        fflush(stdout);
        if (on_change)
            on_change();
        return true;
    }
    return false;
}

void TagPicker::accessibility_parts(Vector<Accessibility::Node>& parts)
{
    auto tags = Desktop::standard_tags();
    for (size_t i = 0; i < tags.size(); ++i) {
        bool on = false;
        for (auto const& tag : m_tags)
            on = on || tag.view() == tags[i].name;
        parts.append(Accessibility::part_of(*this, Accessibility::Role::Checkbox, tags[i].name, dot_rect(i), on ? "on"_sv : "off"_sv));
    }
}

// ---- The path bar

void PathBar::set_path(StringView path)
{
    m_crumbs.clear();
    if (is_smart_path(path)) {
        m_crumbs.append(Crumb { MUST(String::from_view(smart_name(path))), MUST(String::from_view(path)), {} });
    } else {
        m_crumbs.append(Crumb { MUST(String::from_view("Vulture HD"_sv)), MUST(String::from_view("/"_sv)), {} });
        size_t start = 1;
        for (size_t i = 1; i <= path.length(); ++i) {
            if (i == path.length() || path[i] == '/') {
                if (i > start)
                    m_crumbs.append(Crumb { MUST(String::from_view(path.substring_view(start, i - start))), MUST(String::from_view(path.substring_view(0, i))), {} });
                start = i + 1;
            }
        }
    }
    set_needs_display();
}

FloatSize PathBar::intrinsic_size() const
{
    return { -1, 24 };
}

void PathBar::draw(Graphics& graphics)
{
    auto const& t = tokens();
    graphics.fill({ 0, 0, width(), height() }, t.color("windowBackground"_sv));
    graphics.fill({ 0, 0, width(), graphics.pixel() }, t.color("separator"_sv));
    auto font = t.named_font("small"_sv);
    float const cap = Appearance::cap_height(font);
    float x = 10;
    int const scale = static_cast<int>(graphics.scale());
    for (size_t i = 0; i < m_crumbs.size(); ++i) {
        auto& crumb = m_crumbs[i];
        auto icon = Desktop::icon_named(i == 0 ? "Drive"_sv : "Folder"_sv, 16, scale);
        if (icon)
            graphics.painter().draw_bitmap(*icon, FloatRect { x, (height() - 16) / 2, 16, 16 });
        float const text = Appearance::text_width(font, crumb.name.view());
        graphics.draw_text(crumb.name.view(), font, { x + 20, (height() + cap) / 2 }, t.color("label"_sv));
        crumb.rect = { x, 0, 20 + text, height() };
        x += 20 + text + 6;
        if (i + 1 < m_crumbs.size()) {
            graphics.draw_symbol(Symbol::ChevronRight, { x - 3, (height() - 10) / 2, 10, 10 }, t.color("tertiaryLabel"_sv));
            x += 12;
        }
    }
}

bool PathBar::mouse_down(MouseEvent const& event)
{
    for (auto const& crumb : m_crumbs) {
        if (crumb.rect.contains(event.position)) {
            if (on_navigate) {
                auto path = crumb.path;
                on_navigate(path.view());
            }
            return true;
        }
    }
    return false;
}

void PathBar::accessibility_parts(Vector<Accessibility::Node>& parts)
{
    for (auto const& crumb : m_crumbs)
        parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, crumb.name.view(), crumb.rect));
}

// ---- The preview pane

void PreviewPane::set_path(StringView path)
{
    m_picture = nullptr;
    auto const generation = ++m_generation;
    m_item = path.is_empty() ? Optional<FileItem> {} : item_at_path(path);
    if (m_item.has_value() && !m_item->is_directory) {
        Desktop::request_preview(path, static_cast<int>(preview_width) * 2, static_cast<int>(preview_width) * 2, [this, generation](RefPtr<Bitmap> picture) {
            if (generation != m_generation)
                return;
            m_picture = move(picture);
            set_needs_display();
        });
    }
    update_accessories();
    set_needs_layout();
    set_needs_display();
}

// The tags and the buttons under it, for what's shown (if they've been made yet: they're made when it's first laid out).
void PreviewPane::update_accessories()
{
    if (!m_tags)
        return;
    m_tags->set_hidden(!m_item.has_value());
    if (m_item.has_value())
        m_tags->set_path(m_item->path.view());
    bool const is_png = m_item.has_value() && Desktop::content_type_of_path(m_item->path.view()) == "public.png"_sv;
    m_rotate_left->set_hidden(!is_png);
    m_rotate_right->set_hidden(!is_png);
}

FloatSize PreviewPane::intrinsic_size() const
{
    return { preview_width, -1 };
}

void PreviewPane::layout()
{
    if (!m_tags) {
        m_tags = &add<TagPicker>();
        m_tags->on_change = [this] {
            if (on_changed)
                on_changed();
        };
        m_tags->set_hidden(true);
        auto rotate = [this](bool clockwise) {
            if (!m_item.has_value())
                return;
            auto path = m_item->path;
            rotate_picture(path.view(), clockwise, [this, path] {
                set_path(path.view());
                if (on_changed)
                    on_changed();
            });
        };
        m_rotate_left = &add<Button>("Rotate Left"_sv);
        m_rotate_left->on_action = [rotate] { rotate(false); };
        m_rotate_left->set_hidden(true);
        m_rotate_right = &add<Button>("Rotate Right"_sv);
        m_rotate_right->on_action = [rotate] { rotate(true); };
        m_rotate_right->set_hidden(true);
        update_accessories();
    }
    float const inset = 12;
    float y = height() - 34;
    m_rotate_left->set_frame({ inset, y, (width() - inset * 2 - 8) / 2, 22 });
    m_rotate_right->set_frame({ inset + (width() - inset * 2 - 8) / 2 + 8, y, (width() - inset * 2 - 8) / 2, 22 });
    m_tags->set_frame({ inset, m_rotate_left->is_hidden() ? y : y - 34, width() - inset * 2, 22 });
}

void PreviewPane::draw(Graphics& graphics)
{
    auto const& t = tokens();
    graphics.fill({ 0, 0, width(), height() }, t.color("textBackground"_sv));
    graphics.fill({ 0, 0, graphics.pixel(), height() }, t.color("separator"_sv));
    if (!m_item.has_value()) {
        graphics.draw_text_centered("No Selection"_sv, t.named_font("title3"_sv), { 0, 0, width(), height() }, t.color("tertiaryLabel"_sv));
        return;
    }
    float const inset = 12;
    float const text_width = width() - inset * 2;
    // The picture, as big as leaves room under it for what's said about it (wider than tall, in columns).
    constexpr float room_for_text = 140;
    float const below = (m_tags && !m_tags->is_hidden() ? 34 : 0) + (m_rotate_left && !m_rotate_left->is_hidden() ? 34 : 0);
    float const side = max(64.f, min(text_width, height() - inset * 2 - room_for_text - below));
    float shown_height = 128;
    if (m_picture) {
        float const fit = min(side / static_cast<float>(m_picture->width()), side / static_cast<float>(m_picture->height()));
        float const shown_width = static_cast<float>(m_picture->width()) * fit;
        shown_height = static_cast<float>(m_picture->height()) * fit;
        graphics.painter().draw_bitmap(*m_picture, graphics.snapped({ (width() - shown_width) / 2, inset, shown_width, shown_height }));
    } else if (auto icon = Desktop::icon_named(m_item->icon.view(), 128, static_cast<int>(graphics.scale()))) {
        graphics.painter().draw_bitmap(*icon, FloatRect { (width() - 128) / 2, inset, 128, 128 });
    }
    float y = inset + shown_height + 12;
    auto title = t.named_font("title3"_sv);
    auto small = t.named_font("small"_sv);
    graphics.draw_text_truncated(m_item->name.view(), title, { inset, y + 14 }, text_width, t.color("label"_sv));
    y += 22;
    StringBuilder kind;
    kind.appendff("{} \xE2\x80\x94 {}", Desktop::content_description_of_path(m_item->path.view()), m_item->is_directory ? "--"_sv : size_text(m_item->size).view());
    graphics.draw_text_truncated(kind.view(), small, { inset, y + 12 }, text_width, t.color("secondaryLabel"_sv));
    y += 26;
    graphics.draw_text("Information"_sv, small, { inset, y + 10 }, t.color("label"_sv));
    y += 18;
    auto row = [&](StringView label, StringView value) {
        graphics.draw_text(label, small, { inset, y + 12 }, t.color("secondaryLabel"_sv));
        float const value_width = min(Appearance::text_width(small, value), text_width - 70);
        graphics.draw_text_truncated(value, small, { width() - inset - value_width, y + 12 }, value_width, t.color("label"_sv));
        y += 18;
    };
    row("Created"_sv, date_text(m_item->created).view());
    row("Modified"_sv, date_text(m_item->modified).view());
}

// ---- Columns

ColumnBrowser::ColumnBrowser()
{
    m_preview = &add<PreviewPane>();
    m_preview->set_hidden(true);
}

void ColumnBrowser::set_root(StringView path)
{
    m_columns.clear();
    Column root;
    root.path = MUST(String::from_view(path));
    root.items = list_folder(path);
    m_columns.append(move(root));
    m_scroll = 0;
    m_preview->set_hidden(true);
    set_needs_display();
}

void ColumnBrowser::refresh()
{
    for (auto& column : m_columns) {
        String chosen;
        if (column.selected >= 0 && static_cast<size_t>(column.selected) < column.items.size())
            chosen = column.items[static_cast<size_t>(column.selected)].path;
        column.items = list_folder(column.path.view());
        column.selected = -1;
        for (size_t i = 0; i < column.items.size(); ++i) {
            if (column.items[i].path.view() == chosen.view())
                column.selected = static_cast<int>(i);
        }
    }
    set_needs_display();
}

int ColumnBrowser::rightmost_with_selection() const
{
    for (size_t i = m_columns.size(); i > 0; --i) {
        if (m_columns[i - 1].selected >= 0)
            return static_cast<int>(i - 1);
    }
    return -1;
}

Vector<String> ColumnBrowser::selected_paths() const
{
    Vector<String> paths;
    int const column = rightmost_with_selection();
    if (column >= 0)
        paths.append(m_columns[static_cast<size_t>(column)].items[static_cast<size_t>(m_columns[static_cast<size_t>(column)].selected)].path);
    return paths;
}

float ColumnBrowser::column_x(size_t index) const
{
    return static_cast<float>(index) * column_width - m_scroll;
}

void ColumnBrowser::bring_into_view(size_t column)
{
    float const preview = m_preview->is_hidden() ? 0 : preview_width;
    float const right = static_cast<float>(column + 1) * column_width + preview;
    if (right - m_scroll > width())
        m_scroll = right - width();
    if (static_cast<float>(column) * column_width < m_scroll)
        m_scroll = static_cast<float>(column) * column_width;
}

void ColumnBrowser::select(size_t column_index, int row)
{
    while (m_columns.size() > column_index + 1)
        m_columns.remove(m_columns.size() - 1);
    auto& column = m_columns[column_index];
    column.selected = row;
    m_preview->set_hidden(true);
    if (row >= 0) {
        auto const& item = column.items[static_cast<size_t>(row)];
        if (item.is_directory && !item.name.view().ends_with(".app"_sv)) {
            Column next;
            next.path = item.path;
            next.items = list_folder(item.path.view());
            m_columns.append(move(next));
            bring_into_view(column_index + 1);
        } else {
            m_preview->set_hidden(false);
            m_preview->set_path(item.path.view());
            bring_into_view(column_index);
        }
    }
    set_needs_layout();
    set_needs_display();
    if (on_selection_change)
        on_selection_change();
}

void ColumnBrowser::draw(Graphics& graphics)
{
    auto const& t = tokens();
    graphics.fill({ 0, 0, width(), height() }, t.color("textBackground"_sv));
    float const row_height = t.metric("table.rowHeight"_sv);
    float const icon_size = t.metric("files.listIconSize"_sv);
    auto font = t.named_font("body"_sv);
    float const cap = Appearance::cap_height(font);
    int const scale = static_cast<int>(graphics.scale());
    bool const key = window() && window()->is_key();
    int const focus_column = rightmost_with_selection();
    for (size_t c = 0; c < m_columns.size(); ++c) {
        auto const& column = m_columns[c];
        float const x = column_x(c);
        graphics.save();
        graphics.clip({ x, 0, column_width, height() });
        for (size_t i = 0; i < column.items.size(); ++i) {
            auto const& item = column.items[i];
            float const y = static_cast<float>(i) * row_height - column.scroll + 4;
            if (y + row_height < 0 || y > height())
                continue;
            bool const selected = static_cast<int>(i) == column.selected;
            bool const active = selected && key && static_cast<int>(c) == focus_column;
            if (selected)
                graphics.fill_rounded({ x + 5, y, column_width - 10, row_height }, 5, t.color(active ? "selectedContent"_sv : "selectedContentInactive"_sv));
            if (auto icon = Desktop::icon_named(item.icon.view(), static_cast<int>(icon_size), scale))
                graphics.painter().draw_bitmap(*icon, FloatRect { x + 12, y + (row_height - icon_size) / 2, icon_size, icon_size });
            auto color = t.color(active ? "selectedContentText"_sv : "label"_sv);
            graphics.draw_text_truncated(item.name.view(), font, { x + 16 + icon_size, y + (row_height + cap) / 2 }, column_width - 56 - icon_size, color);
            if (!item.tags.is_empty())
                draw_tag_dots(graphics, item.tags, { x + column_width - 26, y + row_height / 2 }, 8);
            if (item.is_directory && !item.name.view().ends_with(".app"_sv))
                graphics.draw_symbol(Symbol::ChevronRight, { x + column_width - 22, y + (row_height - 12) / 2, 12, 12 }, t.color(active ? "selectedContentText"_sv : "tertiaryLabel"_sv));
        }
        graphics.restore();
        graphics.fill({ x + column_width - graphics.pixel(), 0, graphics.pixel(), height() }, t.color("separator"_sv));
    }
    if (!m_preview->is_hidden())
        m_preview->set_frame({ column_x(m_columns.size()), 0, max(preview_width, width() - column_x(m_columns.size())), height() });
}

bool ColumnBrowser::mouse_down(MouseEvent const& event)
{
    size_t const column_index = static_cast<size_t>(max(0.f, (event.position.x + m_scroll) / column_width));
    if (column_index >= m_columns.size())
        return false;
    window()->make_first_responder(this);
    auto const& column = m_columns[column_index];
    float const row_height = tokens().metric("table.rowHeight"_sv);
    int const row = static_cast<int>((event.position.y + column.scroll - 4) / row_height);
    if (row < 0 || static_cast<size_t>(row) >= column.items.size()) {
        select(column_index, -1);
        return true;
    }
    select(column_index, row);
    auto path = m_columns[column_index].items[static_cast<size_t>(row)].path;
    if (event.is_right_click() && on_context_menu) {
        on_context_menu(event, path.view());
        return false;
    }
    if (event.click_count == 2 && on_open)
        on_open(path.view());
    return true;
}

bool ColumnBrowser::scroll_wheel(ScrollEvent const& event)
{
    m_scroll = max(0.f, m_scroll + event.delta_x);
    size_t const column_index = static_cast<size_t>(max(0.f, (event.position.x + m_scroll) / column_width));
    if (column_index < m_columns.size()) {
        auto& column = m_columns[column_index];
        float const total = static_cast<float>(column.items.size()) * tokens().metric("table.rowHeight"_sv) + 8;
        column.scroll = clamp(column.scroll + event.delta_y, 0.f, max(0.f, total - height()));
    }
    set_needs_display();
    return true;
}

bool ColumnBrowser::key_down(KeyEvent const& event)
{
    int const current = rightmost_with_selection();
    if (event.code == KeySpace) {
        if (on_quick_view)
            on_quick_view();
        return true;
    }
    size_t const column_index = current < 0 ? 0 : static_cast<size_t>(current);
    auto& column = m_columns[column_index];
    switch (event.code) {
    case KeyDown:
        if (column.selected + 1 < static_cast<int>(column.items.size()))
            select(column_index, column.selected + 1);
        return true;
    case KeyUp:
        if (column.selected > 0)
            select(column_index, column.selected - 1);
        return true;
    case KeyRight:
        if (column_index + 1 < m_columns.size() && !m_columns[column_index + 1].items.is_empty())
            select(column_index + 1, 0);
        return true;
    case KeyLeft:
        if (column_index > 0) {
            select(column_index - 1, m_columns[column_index - 1].selected);
            m_columns[column_index].selected = -1;
        }
        return true;
    default:
        return false;
    }
}

void ColumnBrowser::accessibility_parts(Vector<Accessibility::Node>& parts)
{
    float const row_height = tokens().metric("table.rowHeight"_sv);
    for (size_t c = 0; c < m_columns.size(); ++c) {
        auto const& column = m_columns[c];
        for (size_t i = 0; i < column.items.size(); ++i) {
            float const y = static_cast<float>(i) * row_height - column.scroll + 4;
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Cell, column.items[i].name.view(), { column_x(c), y, column_width, row_height }, static_cast<int>(i) == column.selected ? "selected"_sv : StringView {}));
        }
    }
}

// ---- Gallery

GalleryView::GalleryView()
{
    m_info = &add<PreviewPane>();
}

void GalleryView::set_items(Vector<FileItem>* items)
{
    m_items = items;
    m_strip_scroll = 0;
    if (m_items && !m_items->is_empty() && selected() < 0)
        (*m_items)[0].selected = true;
    load_picture();
}

int GalleryView::selected() const
{
    if (!m_items)
        return -1;
    for (size_t i = 0; i < m_items->size(); ++i) {
        if ((*m_items)[i].selected)
            return static_cast<int>(i);
    }
    return -1;
}

void GalleryView::load_picture()
{
    m_picture = nullptr;
    auto const generation = ++m_generation;
    int const index = selected();
    if (index >= 0) {
        auto const& item = (*m_items)[static_cast<size_t>(index)];
        m_info->set_path(item.path.view());
        if (!item.is_directory) {
            Desktop::request_preview(item.path.view(), 1600, 1600, [this, generation](RefPtr<Bitmap> picture) {
                if (generation != m_generation)
                    return;
                m_picture = move(picture);
                set_needs_display();
            });
        }
    } else {
        m_info->set_path({});
    }
    set_needs_display();
}

void GalleryView::refresh_selection()
{
    load_picture();
}

void GalleryView::select_index(size_t index)
{
    if (!m_items || index >= m_items->size())
        return;
    for (auto& item : *m_items)
        item.selected = false;
    (*m_items)[index].selected = true;
    auto const cell = thumbnail_rect(index);
    float const usable = width() - preview_width;
    if (cell.right() > usable)
        m_strip_scroll += cell.right() - usable + 8;
    if (cell.x < 0)
        m_strip_scroll = max(0.f, m_strip_scroll + cell.x - 8);
    load_picture();
    if (on_selection_change)
        on_selection_change();
}

void GalleryView::layout()
{
    m_info->set_frame({ width() - preview_width, 0, preview_width, height() });
}

FloatRect GalleryView::thumbnail_rect(size_t index) const
{
    return { 8 + static_cast<float>(index) * strip_cell - m_strip_scroll, height() - strip_height + 6, strip_cell - 6, strip_height - 12 };
}

void GalleryView::draw(Graphics& graphics)
{
    auto const& t = tokens();
    graphics.fill({ 0, 0, width(), height() }, t.color("textBackground"_sv));
    float const usable = width() - preview_width;
    FloatRect const stage { 16, 16, usable - 32, height() - strip_height - 32 };
    int const scale = static_cast<int>(graphics.scale());
    int const chosen = selected();
    if (m_picture) {
        float const fit = min(min(stage.width / static_cast<float>(m_picture->width()), stage.height / static_cast<float>(m_picture->height())), 1.f);
        float const shown_width = static_cast<float>(m_picture->width()) * fit;
        float const shown_height = static_cast<float>(m_picture->height()) * fit;
        graphics.painter().draw_bitmap(*m_picture, graphics.snapped({ stage.x + (stage.width - shown_width) / 2, stage.y + (stage.height - shown_height) / 2, shown_width, shown_height }));
    } else if (chosen >= 0) {
        if (auto icon = Desktop::icon_named((*m_items)[static_cast<size_t>(chosen)].icon.view(), 128, scale))
            graphics.painter().draw_bitmap(*icon, FloatRect { stage.x + (stage.width - 128) / 2, stage.y + (stage.height - 128) / 2, 128, 128 });
    }
    graphics.fill({ 0, height() - strip_height, usable, graphics.pixel() }, t.color("separator"_sv));
    if (!m_items)
        return;
    graphics.save();
    graphics.clip({ 0, height() - strip_height, usable, strip_height });
    for (size_t i = 0; i < m_items->size(); ++i) {
        auto const& item = (*m_items)[i];
        auto cell = thumbnail_rect(i);
        if (cell.right() < 0 || cell.x > usable)
            continue;
        if (item.selected)
            graphics.fill_rounded(cell, 6, t.color("files.selection"_sv));
        float const side = min(cell.width, cell.height) - 8;
        FloatRect const picture { cell.x + (cell.width - side) / 2, cell.y + (cell.height - side) / 2, side, side };
        Bitmap const* thumbnail = item.has_thumbnail ? Thumbnails::the().get(item, static_cast<int>(side) * scale) : nullptr;
        if (thumbnail) {
            float const fit = min(side / static_cast<float>(thumbnail->width()), side / static_cast<float>(thumbnail->height()));
            float const w = static_cast<float>(thumbnail->width()) * fit;
            float const h = static_cast<float>(thumbnail->height()) * fit;
            graphics.painter().draw_bitmap(*thumbnail, FloatRect { picture.x + (side - w) / 2, picture.y + (side - h) / 2, w, h });
        } else if (auto icon = Desktop::icon_named(item.icon.view(), static_cast<int>(side), scale)) {
            graphics.painter().draw_bitmap(*icon, picture);
        }
    }
    graphics.restore();
}

bool GalleryView::mouse_down(MouseEvent const& event)
{
    if (!m_items)
        return false;
    window()->make_first_responder(this);
    for (size_t i = 0; i < m_items->size(); ++i) {
        if (!thumbnail_rect(i).contains(event.position))
            continue;
        select_index(i);
        if (event.click_count == 2 && on_open) {
            auto path = (*m_items)[i].path;
            on_open(path.view());
        }
        return true;
    }
    return false;
}

bool GalleryView::key_down(KeyEvent const& event)
{
    if (!m_items || m_items->is_empty())
        return false;
    int const current = max(selected(), 0);
    switch (event.code) {
    case KeyLeft:
        if (current > 0)
            select_index(static_cast<size_t>(current - 1));
        return true;
    case KeyRight:
        if (static_cast<size_t>(current + 1) < m_items->size())
            select_index(static_cast<size_t>(current + 1));
        return true;
    case KeySpace:
        if (on_quick_view)
            on_quick_view();
        return true;
    default:
        return false;
    }
}

void GalleryView::accessibility_parts(Vector<Accessibility::Node>& parts)
{
    if (!m_items)
        return;
    for (size_t i = 0; i < m_items->size(); ++i)
        parts.append(Accessibility::part_of(*this, Accessibility::Role::Cell, (*m_items)[i].name.view(), thumbnail_rect(i), (*m_items)[i].selected ? "selected"_sv : StringView {}));
}

// ---- Get Info

namespace {

class InfoContent final : public View {
public:
    void draw(Graphics& graphics) override { graphics.fill({ 0, 0, width(), height() }, tokens().color("windowBackground"_sv)); }
    void layout() override
    {
        for (auto const& child : children())
            child->set_frame({ 0, 0, width(), height() });
    }
};

Vector<OwnPtr<UI::Window>> s_info_windows;

// "Kind:" and what it is, beside it. One thing to those who can't see it, as on a Mac, where the first is the second's title.
class InfoRow final : public View {
public:
    InfoRow(StringView label, StringView value)
        : m_label(MUST(String::from_view(label)))
        , m_value(MUST(String::from_view(value)))
    {
    }

    [[nodiscard]] FloatSize intrinsic_size() const override { return { -1, 16 }; }
    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        auto const font = t.named_font("small"_sv);
        float const baseline = (height() + Appearance::cap_height(font)) / 2;
        constexpr float label_width = 70;
        constexpr float gap = 6;
        float const label_text_width = min(Appearance::text_width(font, m_label.view()), label_width);
        graphics.draw_text_truncated(m_label.view(), font, { label_width - label_text_width, baseline }, label_width, t.color("secondaryLabel"_sv));
        graphics.draw_text_truncated(m_value.view(), font, { label_width + gap, baseline }, max(0.f, width() - label_width - gap), t.color("label"_sv));
    }
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::StaticText; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_label.view())); }
    [[nodiscard]] String accessibility_value() const override { return MUST(String::from_view(m_value.view())); }

private:
    String m_label;
    String m_value;
};

void add_info_row(StackView& stack, StringView label, StringView value)
{
    auto& row = stack.add<InfoRow>(label, value);
    row.set_fixed_height(16);
}

String permissions_text(u32 mode)
{
    bool const can_write = mode & 0200;
    return MUST(String::from_view(can_write ? "Read & Write"_sv : "Read only"_sv));
}

}

void show_get_info(StringView path, Function<void()> on_change)
{
    auto item = item_at_path(path);
    if (!item.has_value())
        return;
    auto const& t = tokens();
    int const offset = static_cast<int>(s_info_windows.size()) * 24;
    StringBuilder title;
    title.appendff("{} Info", item->name.view());
    auto window = adopt_own(*new UI::Window(title.view(), { 80 + offset, 60 + offset, 300, 470 }, WS::WindowTitled));
    auto& root = window->set_content<InfoContent>();
    auto& stack = root.add<StackView>(Axis::Vertical, 8, Alignment::Fill, Insets::all(14));

    auto& header = stack.add<StackView>(Axis::Horizontal, 10, Alignment::Center);
    class IconView final : public View {
    public:
        explicit IconView(StringView name)
            : m_name(MUST(String::from_view(name)))
        {
        }
        void draw(Graphics& graphics) override
        {
            if (auto icon = Desktop::icon_named(m_name.view(), 48, static_cast<int>(graphics.scale())))
                graphics.painter().draw_bitmap(*icon, FloatRect { 0, 0, 48, 48 });
        }

    private:
        String m_name;
    };
    auto& icon = header.add<IconView>(item->icon.view());
    icon.set_fixed_size({ 48, 48 });
    auto& names = header.add<StackView>(Axis::Vertical, 2, Alignment::Fill);
    names.set_stretch(1, 0);
    (void)names.add<Label>(item->name.view(), t.named_font("headline"_sv));
    auto& size_label = names.add<Label>(item->is_directory ? "--"_sv : size_text(item->size).view(), t.named_font("small"_sv));
    size_label.set_style(LabelStyle::Secondary);
    header.set_fixed_height(52);

    auto& tags_label = stack.add<Label>("Tags"_sv, t.named_font("headline"_sv));
    tags_label.set_fixed_height(16);
    auto& picker = stack.add<TagPicker>();
    picker.set_fixed_height(22);
    picker.set_path(path);
    picker.on_change = move(on_change);

    auto& general_label = stack.add<Label>("General"_sv, t.named_font("headline"_sv));
    general_label.set_fixed_height(16);
    add_info_row(stack, "Kind:"_sv, Desktop::content_description_of_path(path));
    add_info_row(stack, "Size:"_sv, item->is_directory ? "--"_sv : size_text(item->size).view());
    add_info_row(stack, "Where:"_sv, parent_of(path).view());
    add_info_row(stack, "Created:"_sv, date_text(item->created).view());
    add_info_row(stack, "Modified:"_sv, date_text(item->modified).view());

    if (!item->is_directory) {
        auto applications = Desktop::applications_for_path(path);
        auto& open_label = stack.add<Label>("Open with"_sv, t.named_font("headline"_sv));
        open_label.set_fixed_height(16);
        auto& row = stack.add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& popup = row.add<PopUpButton>();
        popup.set_accessibility_label("Open with"_sv);
        popup.set_stretch(1, 0);
        for (auto const& application : applications)
            popup.add_item(application.name.view());
        if (applications.is_empty())
            popup.add_item("No Applications"_sv);
        auto& change_all = row.add<Button>("Change All\xE2\x80\xA6"_sv);
        change_all.set_enabled(!applications.is_empty());
        auto type = MUST(String::from_view(Desktop::content_type_of_path(path)));
        auto* popup_pointer = &popup;
        change_all.on_action = [applications, type, popup_pointer] {
            int const index = popup_pointer->selected_index();
            if (index < 0 || static_cast<size_t>(index) >= applications.size())
                return;
            Desktop::set_default_application(type.view(), applications[static_cast<size_t>(index)].identifier.view());
            printf("files: default application changed\n");
            fflush(stdout);
        };
        row.set_fixed_height(24);
    }

    struct stat information {};
    (void)lstat(item->path.characters(), &information);
    auto& sharing = stack.add<Label>("Sharing & Permissions"_sv, t.named_font("headline"_sv));
    sharing.set_fixed_height(16);
    add_info_row(stack, "You can:"_sv, permissions_text(information.st_mode).view());

    auto* raw = window.ptr();
    window->on_closed = [raw] {
        Platform::the().defer([raw] {
            s_info_windows.remove_all_matching([raw](auto const& candidate) { return candidate.ptr() == raw; });
        });
    };
    window->show();
    printf("files: info for %s\n", item->name.characters());
    fflush(stdout);
    s_info_windows.append(move(window));
}

}
