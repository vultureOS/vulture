/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibUI/Menu.h>
#include <LibUI/TableView.h>
#include <LibUI/Window.h>

namespace UI {

using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

}

// ---- Header

void TableHeaderView::add_column(StringView title, float column_width)
{
    m_columns.append(Column { MUST(String::from_view(title)), column_width });
    set_needs_display();
}

float TableHeaderView::column_x(size_t index) const
{
    float x = 0;
    for (size_t i = 0; i < index; ++i)
        x += m_columns[i].width;
    return x;
}

float TableHeaderView::total_width() const
{
    return column_x(m_columns.size());
}

void TableHeaderView::set_scroll(float x)
{
    if (x == m_scroll)
        return;
    m_scroll = x;
    set_needs_display();
}

FloatSize TableHeaderView::intrinsic_size() const
{
    return { -1, tokens().metric("table.headerHeight"_sv) };
}

void TableHeaderView::draw(Graphics& graphics)
{
    auto const& t = tokens();
    graphics.fill({ 0, 0, width(), height() }, t.color("tableHeader.fill"_sv));
    FontSpec font { t.metric("table.headerFontSize"_sv), FontWeight::Medium, false };
    float padding = t.metric("table.headerPadding"_sv);
    float baseline = (height() + Appearance::cap_height(font)) / 2 - 0.5f;
    for (size_t i = 0; i < m_columns.size(); ++i) {
        float x = column_x(i) - m_scroll;
        float column_width = m_columns[i].width;
        if (static_cast<int>(i) == m_pressed)
            graphics.fill({ x, 0, column_width, height() }, t.color("texturedPressed"_sv));
        graphics.draw_text_truncated(m_columns[i].title.view(), font, { x + padding, baseline }, column_width - padding * 2, t.color("tableHeader.text"_sv));
        if (static_cast<int>(i) == m_sort_column) {
            float size = 10;
            graphics.draw_symbol(m_sort_ascending ? Symbol::ChevronUp : Symbol::ChevronDown, { x + column_width - padding - size, (height() - size) / 2, size, size }, t.color("secondaryLabel"_sv), 2);
        }
        float divider_x = graphics.snap(x + column_width + t.metric("table.intercellWidth"_sv) - 1);
        graphics.fill({ divider_x, 6, 1, height() - 13 }, t.color("tableHeader.divider"_sv));
    }
    graphics.fill({ 0, height() - 1, width(), 1 }, t.color("tableHeader.border"_sv));
}

int TableHeaderView::divider_at(float x) const
{
    for (size_t i = 0; i < m_columns.size(); ++i) {
        float divider = column_x(i) + m_columns[i].width - m_scroll;
        if (x > divider - 4 && x < divider + 4)
            return static_cast<int>(i);
    }
    return -1;
}

WS::CursorShape TableHeaderView::cursor() const
{
    return (m_over_divider || m_resizing >= 0) ? WS::CursorShape::ResizeLeftRight : WS::CursorShape::Arrow;
}

void TableHeaderView::mouse_moved(MouseEvent const& event)
{
    m_over_divider = divider_at(event.position.x) >= 0;
}

bool TableHeaderView::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft)
        return false;
    int divider = divider_at(event.position.x);
    if (divider >= 0) {
        m_resizing = divider;
        m_resize_start_x = event.position.x;
        m_resize_start_width = m_columns[static_cast<size_t>(divider)].width;
        return true;
    }
    for (size_t i = 0; i < m_columns.size(); ++i) {
        float x = column_x(i) - m_scroll;
        if (event.position.x >= x && event.position.x < x + m_columns[i].width) {
            m_pressed = static_cast<int>(i);
            set_needs_display();
            return true;
        }
    }
    return true;
}

void TableHeaderView::mouse_dragged(MouseEvent const& event)
{
    if (m_resizing < 0)
        return;
    float new_width = max(m_resize_start_width + event.position.x - m_resize_start_x, 24.0f);
    m_columns[static_cast<size_t>(m_resizing)].width = new_width;
    set_needs_display();
    if (on_columns_changed)
        on_columns_changed();
}

void TableHeaderView::mouse_up(MouseEvent const& event)
{
    m_resizing = -1;
    if (m_pressed >= 0 && event.position.y >= 0 && event.position.y < height()) {
        auto column = static_cast<size_t>(m_pressed);
        bool ascending = m_sort_column == m_pressed ? !m_sort_ascending : true;
        m_sort_column = m_pressed;
        m_sort_ascending = ascending;
        if (on_sort)
            on_sort(column, ascending);
    }
    m_pressed = -1;
    set_needs_display();
}

// ---- Rows

class RowsView final : public View {
public:
    explicit RowsView(TableView& table)
        : m_table(table)
    {
    }

    [[nodiscard]] FloatSize intrinsic_size() const override
    {
        size_t rows = m_table.m_model ? m_table.m_model->row_count() : 0;
        auto const& t = tokens();
        float row_height = t.metric("table.rowHeight"_sv);
        return { m_table.m_header->total_width(), static_cast<float>(rows) * row_height };
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        auto* model = m_table.m_model;
        float row_height = t.metric("table.rowHeight"_sv);
        auto clip = graphics.painter().clip_bounds().to<float>();
        auto origin = graphics.painter().transform().map(FloatPoint { 0, 0 });
        float scale = graphics.scale();
        float visible_top = (clip.y - origin.y) / scale;
        float visible_bottom = (clip.bottom() - origin.y) / scale;
        size_t rows = model ? model->row_count() : 0;
        int first = max(static_cast<int>(visible_top / row_height), 0);
        // Stripes carry on past the rows.
        int last = static_cast<int>(visible_bottom / row_height) + 1;

        graphics.fill({ 0, visible_top, width(), visible_bottom - visible_top }, t.color("controlBackground"_sv));
        if (m_table.m_alternating) {
            auto stripe = t.color("alternatingRow"_sv);
            for (int i = first; i <= last; ++i) {
                if (i % 2 == 1)
                    graphics.fill({ 0, static_cast<float>(i) * row_height, width(), row_height }, stripe);
            }
        }
        if (!model)
            return;

        float inset = t.metric("table.selectionInset"_sv);
        float radius = t.metric("table.selectionRadius"_sv);
        float padding = t.metric("table.cellPadding"_sv);
        auto font = t.named_font("body"_sv);
        float cap = Appearance::cap_height(font);
        bool key = m_table.window() && m_table.window()->is_key() && m_table.is_first_responder();
        size_t columns = m_table.m_header->column_count();
        float symbol_size = t.metric("sidebar.iconSize"_sv);
        float symbol_gap = t.metric("sidebar.iconGap"_sv);

        for (int i = first; i <= last && static_cast<size_t>(i) < rows; ++i) {
            auto row = static_cast<size_t>(i);
            float y = static_cast<float>(row) * row_height;
            bool selected = m_table.m_selection.contains_slow(row);
            if (selected) {
                graphics.fill_rounded({ inset, y, width() - inset * 2, row_height }, radius, t.color(key ? "selectedContent"_sv : "selectedContentInactive"_sv));
            }
            auto text_color = t.color(selected && key ? "selectedContentText"_sv : "label"_sv);
            FloatRect row_rect { 0, y, width(), row_height };
            m_table.draw_row_prefix(graphics, row, row_rect);
            for (size_t column = 0; column < columns; ++column) {
                float x = m_table.m_header->column_x(column) + padding;
                float cell_width = m_table.m_header->column_width(column) - padding * 2;
                if (column == 0) {
                    float indent = m_table.indent_of(row);
                    x += indent;
                    cell_width -= indent;
                    if (model->has_symbol_at(row)) {
                        graphics.draw_symbol(model->symbol_at(row), { x, y + (row_height - symbol_size) / 2, symbol_size, symbol_size }, t.color(selected && key ? "selectedContentText"_sv : "sidebar.icon"_sv));
                        x += symbol_size + symbol_gap;
                        cell_width -= symbol_size + symbol_gap;
                    }
                }
                auto text = model->text_at(row, column);
                graphics.draw_text_truncated(text.view(), font, { x, y + (row_height + cap) / 2 }, max(cell_width, 0.0f), text_color);
            }
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        if (event.button != WS::ButtonLeft)
            return false;
        float row_height = tokens().metric("table.rowHeight"_sv);
        int row = static_cast<int>(event.position.y / row_height);
        size_t rows = m_table.m_model ? m_table.m_model->row_count() : 0;
        if (row < 0 || static_cast<size_t>(row) >= rows) {
            m_table.clear_selection();
            return true;
        }
        m_press_row = row;
        m_press_at = event.window_position;
        m_table.row_pressed(static_cast<size_t>(row), event);
        return true;
    }

    void mouse_dragged(MouseEvent const& event) override
    {
        if (m_press_row < 0 || !m_table.on_drag_row || !window())
            return;
        auto moved = event.window_position - m_press_at;
        float reach = window()->click_distance();
        if (moved.x * moved.x + moved.y * moved.y < reach * reach)
            return;
        String type;
        Vector<u8> data;
        auto row = static_cast<size_t>(m_press_row);
        m_press_row = -1;
        if (!m_table.on_drag_row(row, type, data))
            return;
        auto text = m_table.m_model->text_at(row, 0);
        auto font = tokens().named_font("body"_sv);
        float text_width = Appearance::text_width(font, text.view());
        window()->begin_drag(type.view(), data.span(), { text_width + 16, 24 }, [text = move(text), font](Graphics& graphics) {
            auto const& t = tokens();
            graphics.draw_text(text.view(), font, { 8, 16 }, t.color("label"_sv));
        });
    }

    void mouse_up(MouseEvent const&) override { m_press_row = -1; }

private:
    TableView& m_table;
    int m_press_row { -1 };
    FloatPoint m_press_at;
};

// ---- Table

TableView::TableView(TableModel* model)
    : m_model(model)
{
    m_scroll = &add<ScrollView>();
    m_header = &add<TableHeaderView>();
    m_rows = &m_scroll->make_document<RowsView>(*this);
    m_scroll->on_scroll = [this] { m_header->set_scroll(m_scroll->offset().x); };
    m_header->on_columns_changed = [this] {
        m_scroll->set_needs_layout();
        m_rows->set_needs_display();
    };
    m_header->on_sort = [this](size_t column, bool ascending) {
        if (m_model)
            m_model->sort_by(column, ascending);
        m_rows->set_needs_display();
    };
}

TableView::~TableView() = default;

void TableView::set_model(TableModel* model)
{
    m_model = model;
    reload();
}

void TableView::add_column(StringView title, float column_width)
{
    m_header->add_column(title, column_width);
    m_scroll->set_needs_layout();
}

void TableView::set_headers_visible(bool visible)
{
    m_headers_visible = visible;
    m_header->set_hidden(!visible);
    set_needs_layout();
}

void TableView::reload()
{
    m_selection.remove_all_matching([this](size_t row) { return !m_model || row >= m_model->row_count(); });
    m_scroll->set_needs_layout();
    m_rows->set_needs_display();
}

void TableView::set_alternating_rows(bool alternating)
{
    m_alternating = alternating;
    m_rows->set_needs_display();
}

void TableView::layout()
{
    float header_height = m_headers_visible ? tokens().metric("table.headerHeight"_sv) : 0;
    m_header->set_frame({ 0, 0, width(), header_height });
    m_scroll->set_frame({ 0, header_height, width(), height() - header_height });
    m_rows->set_minimum_size({ width(), 0 });
}

void TableView::selection_changed()
{
    m_rows->set_needs_display();
    if (on_selection_change)
        on_selection_change();
}

void TableView::select_row(size_t row, bool extend)
{
    if (!m_model || row >= m_model->row_count())
        return;
    if (!extend || !m_multiple)
        m_selection.clear();
    if (!m_selection.contains_slow(row))
        m_selection.append(row);
    m_anchor = static_cast<int>(row);
    selection_changed();
}

void TableView::clear_selection()
{
    if (m_selection.is_empty())
        return;
    m_selection.clear();
    selection_changed();
}

void TableView::scroll_to_row(size_t row)
{
    float row_height = tokens().metric("table.rowHeight"_sv);
    m_scroll->scroll_rect_into_view({ 0, static_cast<float>(row) * row_height, 1, row_height });
}

void TableView::row_pressed(size_t row, MouseEvent const& event)
{
    if (row_prefix_pressed(row, event.position.x))
        return;
    if (event.click_count == 2) {
        if (on_activate)
            on_activate(row);
        return;
    }
    bool command = event.modifiers & WS::ModifierCommand;
    bool shift = event.modifiers & WS::ModifierShift;
    if (m_multiple && command) {
        if (m_selection.contains_slow(row))
            m_selection.remove_all_matching([row](size_t other) { return other == row; });
        else
            m_selection.append(row);
        m_anchor = static_cast<int>(row);
        selection_changed();
        return;
    }
    if (m_multiple && shift && m_anchor >= 0) {
        m_selection.clear();
        auto from = min(static_cast<size_t>(m_anchor), row);
        auto to = max(static_cast<size_t>(m_anchor), row);
        for (size_t i = from; i <= to; ++i)
            m_selection.append(i);
        selection_changed();
        return;
    }
    select_row(row);
}

bool TableView::key_down(KeyEvent const& event)
{
    if (!m_model || m_model->row_count() == 0)
        return false;
    int current = m_selection.is_empty() ? -1 : static_cast<int>(m_selection.last());
    int count = static_cast<int>(m_model->row_count());
    switch (event.code) {
    case KeyUp:
        current = current <= 0 ? 0 : current - 1;
        break;
    case KeyDown:
        current = current + 1 >= count ? count - 1 : current + 1;
        break;
    case KeyHome:
        current = 0;
        break;
    case KeyEnd:
        current = count - 1;
        break;
    case KeyEnter:
        if (current >= 0 && on_activate) {
            on_activate(static_cast<size_t>(current));
            return true;
        }
        return false;
    default:
        return false;
    }
    select_row(static_cast<size_t>(current), event.has_shift());
    scroll_to_row(static_cast<size_t>(current));
    return true;
}

// ---- Outline

OutlineView::OutlineView(OutlineModel& outline)
    : TableView()
    , m_outline(outline)
{
    set_model(this);
    reload_items();
}

void OutlineView::flatten_into(void* parent, int depth)
{
    size_t count = m_outline.child_count(parent);
    for (size_t i = 0; i < count; ++i) {
        void* item = m_outline.child_at(parent, i);
        m_rows_flat.append(Row { item, depth });
        if (is_expanded(item))
            flatten_into(item, depth + 1);
    }
}

void OutlineView::reload_items()
{
    m_rows_flat.clear();
    flatten_into(nullptr, 0);
    reload();
}

bool OutlineView::is_expanded(void* item) const
{
    return m_expanded.contains_slow(item);
}

void OutlineView::set_expanded(void* item, bool expanded)
{
    if (expanded == is_expanded(item))
        return;
    if (expanded)
        m_expanded.append(item);
    else
        m_expanded.remove_all_matching([item](void* other) { return other == item; });
    reload_items();
}

float OutlineView::indent_of(size_t row) const
{
    auto const& t = tokens();
    return static_cast<float>(m_rows_flat[row].depth) * t.metric("table.outlineIndent"_sv) + t.metric("table.disclosureSize"_sv);
}

void OutlineView::draw_row_prefix(Graphics& graphics, size_t row, FloatRect const& rect)
{
    auto const& entry = m_rows_flat[row];
    if (m_outline.child_count(entry.item) == 0)
        return;
    auto const& t = tokens();
    float size = t.metric("table.disclosureSize"_sv);
    float x = t.metric("table.cellPadding"_sv) + static_cast<float>(entry.depth) * t.metric("table.outlineIndent"_sv);
    graphics.draw_symbol(is_expanded(entry.item) ? Symbol::ChevronDown : Symbol::ChevronRight, { x, rect.y + (rect.height - size) / 2, size, size }, t.color("disclosureSymbol"_sv), 1.8f);
}

bool OutlineView::row_prefix_pressed(size_t row, float x)
{
    auto const& entry = m_rows_flat[row];
    if (m_outline.child_count(entry.item) == 0)
        return false;
    auto const& t = tokens();
    float size = t.metric("table.disclosureSize"_sv);
    float left = t.metric("table.cellPadding"_sv) + static_cast<float>(entry.depth) * t.metric("table.outlineIndent"_sv);
    if (x < left || x >= left + size)
        return false;
    set_expanded(entry.item, !is_expanded(entry.item));
    return true;
}

bool OutlineView::key_down(KeyEvent const& event)
{
    int selected = selected_row();
    if (selected >= 0) {
        void* item = m_rows_flat[static_cast<size_t>(selected)].item;
        if (event.code == KeyRight && m_outline.child_count(item) > 0) {
            set_expanded(item, true);
            return true;
        }
        if (event.code == KeyLeft) {
            if (is_expanded(item)) {
                set_expanded(item, false);
                return true;
            }
            // To its parent.
            int depth = m_rows_flat[static_cast<size_t>(selected)].depth;
            for (int i = selected - 1; i >= 0; --i) {
                if (m_rows_flat[static_cast<size_t>(i)].depth < depth) {
                    select_row(static_cast<size_t>(i));
                    return true;
                }
            }
            return true;
        }
    }
    return TableView::key_down(event);
}

// ---- Collection

CollectionView::CollectionView(CollectionModel& model)
    : m_model(model)
{
}

int CollectionView::columns() const
{
    auto const& t = tokens();
    float item = t.metric("collection.itemWidth"_sv);
    float spacing = t.metric("collection.spacing"_sv);
    return max(static_cast<int>((width() + spacing) / (item + spacing)), 1);
}

FloatRect CollectionView::item_rect(size_t index) const
{
    auto const& t = tokens();
    float item_width = t.metric("collection.itemWidth"_sv);
    float item_height = t.metric("collection.itemHeight"_sv);
    float spacing = t.metric("collection.spacing"_sv);
    int across = columns();
    auto column = static_cast<int>(index) % across;
    auto row = static_cast<int>(index) / across;
    return { static_cast<float>(column) * (item_width + spacing), static_cast<float>(row) * (item_height + spacing), item_width, item_height };
}

FloatSize CollectionView::intrinsic_size() const
{
    auto const& t = tokens();
    int across = columns();
    int rows = (static_cast<int>(m_model.item_count()) + across - 1) / across;
    float item_height = t.metric("collection.itemHeight"_sv);
    float spacing = t.metric("collection.spacing"_sv);
    return { -1, static_cast<float>(rows) * (item_height + spacing) };
}

void CollectionView::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto font = t.named_font("small"_sv);
    float radius = t.metric("collection.radius"_sv);
    float icon = t.metric("collection.iconSize"_sv);
    bool key = window() && window()->is_key() && is_first_responder();
    for (size_t i = 0; i < m_model.item_count(); ++i) {
        auto rect = item_rect(i);
        bool selected = static_cast<int>(i) == m_selected;
        if (selected)
            graphics.fill_rounded(rect, radius, t.color(key ? "selectedContent"_sv : "selectedContentInactive"_sv));
        graphics.draw_symbol(m_model.symbol_of(i), { rect.x + (rect.width - icon) / 2, rect.y + 8, icon, icon }, t.color(selected && key ? "selectedContentText"_sv : "sidebar.icon"_sv));
        auto title = m_model.title_of(i);
        float text_width = min(Appearance::text_width(font, title.view()), rect.width - 8);
        graphics.draw_text_truncated(title.view(), font, { rect.x + (rect.width - text_width) / 2, rect.y + rect.height - 12 }, rect.width - 8, t.color(selected && key ? "selectedContentText"_sv : "label"_sv));
    }
}

void CollectionView::select_item(int index)
{
    if (index == m_selected)
        return;
    m_selected = index;
    set_needs_display();
    if (on_selection_change)
        on_selection_change();
}

bool CollectionView::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft)
        return false;
    for (size_t i = 0; i < m_model.item_count(); ++i) {
        if (item_rect(i).contains(event.position)) {
            select_item(static_cast<int>(i));
            if (event.click_count == 2 && on_activate)
                on_activate(i);
            return true;
        }
    }
    select_item(-1);
    return true;
}

bool CollectionView::key_down(KeyEvent const& event)
{
    int count = static_cast<int>(m_model.item_count());
    if (count == 0)
        return false;
    int across = columns();
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
        if (m_selected >= 0 && on_activate) {
            on_activate(static_cast<size_t>(m_selected));
            return true;
        }
        return false;
    default:
        return false;
    }
    select_item(clamp(next, 0, count - 1));
    return true;
}

}
