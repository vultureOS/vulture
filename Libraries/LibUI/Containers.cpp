/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Containers.h>
#include <LibUI/Window.h>

namespace UI {

using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

}

// ---- Split

SplitView::SplitView(Axis axis, DividerStyle divider)
    : m_axis(axis)
    , m_divider(divider)
{
}

float SplitView::thickness() const
{
    auto const& t = tokens();
    switch (m_divider) {
    case DividerStyle::Thin:
        return t.metric("split.thin"_sv);
    case DividerStyle::Thick:
        return t.metric("split.thick"_sv);
    case DividerStyle::PaneSplitter:
        return t.metric("split.pane"_sv);
    }
    VERIFY_NOT_REACHED();
}

void SplitView::layout()
{
    float total = m_axis == Axis::Horizontal ? width() : height();
    float across = m_axis == Axis::Horizontal ? height() : width();
    float divider = thickness();
    float at = 0;
    for (size_t i = 0; i < m_panes.size(); ++i) {
        auto& pane = m_panes[i];
        bool last = i + 1 == m_panes.size();
        // What the later ones need stays theirs.
        float needed_after = 0;
        for (size_t j = i + 1; j < m_panes.size(); ++j)
            needed_after += m_panes[j].minimum + divider;
        float room = total - at - needed_after;
        float size = last ? max(total - at, 0.0f) : clamp(pane.size, pane.minimum, max(room, pane.minimum));
        if (!last)
            pane.size = size;
        if (m_axis == Axis::Horizontal)
            pane.view->set_frame({ at, 0, size, across });
        else
            pane.view->set_frame({ 0, at, across, size });
        at += size + divider;
    }
}

void SplitView::set_pane_size(size_t index, float size)
{
    m_panes[index].size = size;
    set_needs_layout();
    set_needs_display();
}

void SplitView::draw_over_children(Graphics& graphics)
{
    auto const& t = tokens();
    float divider = thickness();
    float across = m_axis == Axis::Horizontal ? height() : width();
    for (size_t i = 0; i + 1 < m_panes.size(); ++i) {
        auto frame = m_panes[i].view->frame();
        float at = m_axis == Axis::Horizontal ? frame.right() : frame.bottom();
        FloatRect rect = m_axis == Axis::Horizontal ? FloatRect { at, 0, divider, across } : FloatRect { 0, at, across, divider };
        graphics.fill(graphics.snapped(rect), t.color("separator"_sv));
        if (m_divider != DividerStyle::Thin) {
            // A dimple in the middle, to take hold of.
            float size = 5;
            FloatPoint center { rect.x + rect.width / 2, rect.y + rect.height / 2 };
            graphics.fill_circle(center, size / 2, t.color("secondaryLabel"_sv));
        }
    }
}

int SplitView::divider_at(FloatPoint point) const
{
    float grip = max(thickness(), 5.0f);
    for (size_t i = 0; i + 1 < m_panes.size(); ++i) {
        auto frame = m_panes[i].view->frame();
        float at = m_axis == Axis::Horizontal ? frame.right() : frame.bottom();
        float position = m_axis == Axis::Horizontal ? point.x : point.y;
        float middle = at + thickness() / 2;
        if (position >= middle - grip / 2 && position < middle + grip / 2)
            return static_cast<int>(i);
    }
    return -1;
}

View* SplitView::hit_test(FloatPoint in_parent)
{
    if (is_hidden())
        return nullptr;
    auto point = in_parent - frame().origin();
    // A divider's grip is wider than it looks, and over what is beside it.
    if (bounds().contains(point) && divider_at(point) >= 0)
        return this;
    return View::hit_test(in_parent);
}

bool SplitView::mouse_down(MouseEvent const& event)
{
    int divider = divider_at(event.position);
    if (divider < 0 || event.button != WS::ButtonLeft)
        return false;
    m_dragging = divider;
    m_drag_from = m_axis == Axis::Horizontal ? event.position.x : event.position.y;
    m_drag_start_size = m_panes[static_cast<size_t>(divider)].size;
    return true;
}

void SplitView::mouse_dragged(MouseEvent const& event)
{
    if (m_dragging < 0)
        return;
    float at = m_axis == Axis::Horizontal ? event.position.x : event.position.y;
    set_pane_size(static_cast<size_t>(m_dragging), m_drag_start_size + at - m_drag_from);
    if (on_resize_pane)
        on_resize_pane();
}

void SplitView::mouse_up(MouseEvent const&)
{
    m_dragging = -1;
}

void SplitView::mouse_moved(MouseEvent const& event)
{
    m_over_divider = divider_at(event.position) >= 0;
}

WS::CursorShape SplitView::cursor() const
{
    if (m_over_divider || m_dragging >= 0)
        return m_axis == Axis::Horizontal ? WS::CursorShape::ResizeLeftRight : WS::CursorShape::ResizeUpDown;
    return WS::CursorShape::Arrow;
}

// ---- Tabs

TabView::TabView()
{
    m_pages_parent = &add<View>();
    m_strip = &add<SegmentedControl>();
    m_strip->on_change = [this](size_t index) { select_tab(index); };
}

void TabView::register_tab(StringView title, View& page)
{
    m_pages.append(&page);
    m_strip->add_segment(title);
    page.set_hidden(m_pages.size() != 1);
    if (m_pages.size() == 1)
        m_strip->set_selected(0);
    set_needs_layout();
}

void TabView::select_tab(size_t index)
{
    if (index >= m_pages.size() || index == m_selected)
        return;
    m_pages[m_selected]->set_hidden(true);
    m_selected = index;
    m_pages[m_selected]->set_hidden(false);
    m_strip->set_selected(index);
    set_needs_layout();
    set_needs_display();
    if (on_change)
        on_change(index);
}

void TabView::layout()
{
    auto const& t = tokens();
    float left = t.metric("tabs.insetLeft"_sv);
    float top = t.metric("tabs.insetTop"_sv);
    float right = t.metric("tabs.insetRight"_sv);
    float bottom = t.metric("tabs.insetBottom"_sv);
    auto strip_size = m_strip->intrinsic_size();
    float box_top = t.metric("tabs.buttonHeight"_sv) / 2;
    m_strip->set_frame({ (width() - strip_size.width) / 2, 0, strip_size.width, strip_size.height });
    m_pages_parent->set_frame({ left, top, max(width() - left - right, 0.0f), max(height() - top - bottom, 0.0f) });
    (void)box_top;
    for (auto* page : m_pages)
        page->set_frame({ 0, 0, m_pages_parent->width(), m_pages_parent->height() });
}

void TabView::draw(Graphics& graphics)
{
    auto const& t = tokens();
    float box_top = t.metric("tabs.buttonHeight"_sv) / 2;
    FloatRect shape { 0, box_top, width(), height() - box_top };
    float radius = t.metric("tabs.radius"_sv);
    graphics.fill_rounded(shape, radius, t.color("tabs.fill"_sv));
    graphics.stroke_rounded(shape, radius, graphics.pixel(), t.color("tabs.border"_sv));
}

// ---- Toolbar

class ToolbarSpacer final : public View {
public:
    ToolbarSpacer() { set_stretch(1, 0); }
    [[nodiscard]] FloatSize intrinsic_size() const override { return { 0, 0 }; }
};

ToolbarButton::ToolbarButton(Symbol symbol, StringView label)
    : m_symbol(symbol)
    , m_label(MUST(String::from_view(label)))
{
}

bool ToolbarButton::accessibility_press()
{
    if (!is_enabled())
        return false;
    activate();
    return true;
}

FloatSize ToolbarButton::intrinsic_size() const
{
    float size = tokens().metric("toolbar.itemSize"_sv);
    return { size, size };
}

void ToolbarButton::mouse_entered()
{
    Control::mouse_entered();
    set_needs_display();
}

void ToolbarButton::mouse_exited()
{
    Control::mouse_exited();
    set_needs_display();
}

void ToolbarButton::draw(Graphics& graphics)
{
    auto const& t = tokens();
    FloatRect shape { 0, 0, width(), height() };
    float radius = t.metric("toolbar.radius"_sv);
    if (is_pressed())
        graphics.fill_rounded(shape, radius, t.color("toolbar.pressed"_sv));
    else if (is_hovered())
        graphics.fill_rounded(shape, radius, t.color("toolbar.hover"_sv));
    float symbol = t.metric("toolbar.symbolSize"_sv);
    graphics.draw_symbol(m_symbol, { (width() - symbol) / 2, (height() - symbol) / 2, symbol, symbol }, t.color(is_enabled() ? "toolbar.symbol"_sv : "tertiaryLabel"_sv));
}

Toolbar::Toolbar()
{
    auto const& t = tokens();
    m_items = &add<StackView>(Axis::Horizontal, t.metric("toolbar.spacing"_sv), Alignment::Center);
}

void Toolbar::set_style(Style style)
{
    m_style = style;
    if (parent())
        parent()->set_needs_layout();
    set_needs_display();
}

void Toolbar::set_title(StringView title)
{
    m_title = MUST(String::from_view(title));
    set_needs_layout();
    set_needs_display();
}

ToolbarButton& Toolbar::add_button(Symbol symbol, Function<void()> action, StringView label)
{
    auto& button = m_items->add<ToolbarButton>(symbol, label);
    button.on_action = move(action);
    return button;
}

SearchField& Toolbar::add_search(float search_width)
{
    auto& field = m_items->add<SearchField>();
    field.set_fixed_width(search_width);
    return field;
}

SegmentedControl& Toolbar::add_segmented()
{
    return m_items->add<SegmentedControl>();
}

void Toolbar::add_flexible_space()
{
    m_items->add<ToolbarSpacer>();
}

void Toolbar::set_left_padding(float padding)
{
    m_left_padding = padding;
    set_needs_layout();
    set_needs_display();
}

FloatSize Toolbar::intrinsic_size() const
{
    auto const& t = tokens();
    return { -1, t.metric(m_style == Style::Unified ? "window.toolbarUnified"_sv : "window.toolbarCompact"_sv) };
}

void Toolbar::layout()
{
    auto const& t = tokens();
    float left = m_left_padding >= 0 ? m_left_padding : t.metric("toolbar.paddingLeft"_sv);
    if (!m_title.is_empty())
        left += Appearance::text_width(t.named_font("headline"_sv), m_title.view()) + t.metric("toolbar.spacing"_sv) * 2;
    m_items->set_frame({ left, 0, max(width() - left - t.metric("toolbar.paddingRight"_sv), 0.0f), height() });
}

void Toolbar::draw(Graphics& graphics)
{
    auto const& t = tokens();
    graphics.fill({ 0, 0, width(), height() }, t.color("toolbar.fill"_sv));
    graphics.fill({ 0, graphics.snap(height() - 1), width(), 1 }, t.color("toolbar.separator"_sv));
    if (!m_title.is_empty()) {
        auto font = t.named_font("headline"_sv);
        graphics.draw_text(m_title.view(), font, { m_left_padding >= 0 ? m_left_padding : t.metric("toolbar.paddingLeft"_sv), (height() + Appearance::cap_height(font)) / 2 }, t.color(window() && window()->is_key() ? "label"_sv : "tertiaryLabel"_sv));
    }
}

bool Toolbar::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft || !window())
        return false;
    window()->move_by_pointer();
    return true;
}

// ---- Sidebar

Sidebar::Sidebar() = default;

void Sidebar::add_section(StringView title)
{
    m_entries.append(Entry { MUST(String::from_view(title)), Symbol::Folder, true, -1 });
    set_needs_display();
}

size_t Sidebar::add_row(StringView title, Symbol symbol)
{
    size_t row = m_rows++;
    m_entries.append(Entry { MUST(String::from_view(title)), symbol, false, static_cast<int>(row) });
    set_needs_display();
    return row;
}

size_t Sidebar::insert_row_after(size_t row, StringView title, Symbol symbol)
{
    for (auto& entry : m_entries) {
        if (entry.row > static_cast<int>(row))
            ++entry.row;
    }
    if (m_selected > static_cast<int>(row))
        ++m_selected;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].row == static_cast<int>(row)) {
            m_entries.insert(i + 1, Entry { MUST(String::from_view(title)), symbol, false, static_cast<int>(row + 1) });
            break;
        }
    }
    ++m_rows;
    set_needs_display();
    return row + 1;
}

void Sidebar::remove_row(size_t row)
{
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].row == static_cast<int>(row)) {
            m_entries.remove(i);
            break;
        }
    }
    for (auto& entry : m_entries) {
        if (entry.row > static_cast<int>(row))
            --entry.row;
    }
    if (m_selected == static_cast<int>(row))
        m_selected = -1;
    else if (m_selected > static_cast<int>(row))
        --m_selected;
    --m_rows;
    set_needs_display();
}

void Sidebar::clear()
{
    m_entries.clear();
    m_rows = 0;
    m_selected = -1;
    set_needs_display();
}

void Sidebar::set_row_accessory(size_t row, Symbol symbol, StringView label, Function<void()> action)
{
    for (auto& entry : m_entries) {
        if (entry.row == static_cast<int>(row)) {
            entry.has_accessory = true;
            entry.accessory = symbol;
            entry.accessory_label = MUST(String::from_view(label));
            entry.accessory_action = move(action);
        }
    }
    set_needs_display();
}

void Sidebar::accessibility_parts(Vector<Accessibility::Node>& parts)
{
    float const row_height = tokens().metric("sidebar.rowHeight"_sv);
    float const accessory = tokens().metric("sidebar.iconSize"_sv);
    float const inset = tokens().metric("sidebar.rowInset"_sv);
    for (size_t i = 0; i < m_entries.size(); ++i) {
        auto const& entry = m_entries[i];
        if (entry.section)
            parts.append(Accessibility::part_of(*this, Accessibility::Role::StaticText, entry.title.view(), { 0, y_of(i), width(), row_height }));
        else
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Row, entry.title.view(), { 0, y_of(i), width(), row_height }, entry.row == m_selected ? "selected"_sv : StringView {}));
        if (entry.has_accessory)
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, entry.accessory_label.view(), { width() - inset - accessory - 6, y_of(i) + (row_height - accessory) / 2, accessory, accessory }));
    }
}

StringView Sidebar::row_title(size_t row) const
{
    for (auto const& entry : m_entries) {
        if (entry.row == static_cast<int>(row))
            return entry.title.view();
    }
    return {};
}

void Sidebar::set_top_padding(float padding)
{
    m_top_padding = padding;
    set_needs_display();
}

float Sidebar::y_of(size_t index) const
{
    auto const& t = tokens();
    float y = m_top_padding >= 0 ? m_top_padding : t.metric("sidebar.topPadding"_sv);
    float row = t.metric("sidebar.rowHeight"_sv);
    float section = t.metric("sidebar.sectionHeight"_sv);
    for (size_t i = 0; i < index; ++i)
        y += m_entries[i].section ? section : row;
    return y;
}

FloatSize Sidebar::intrinsic_size() const
{
    return { tokens().metric("sidebar.width"_sv), y_of(m_entries.size()) };
}

void Sidebar::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto body = t.named_font("body"_sv);
    auto section_font = t.named_font("small"_sv);
    section_font.weight = FontWeight::SemiBold;
    float row_height = t.metric("sidebar.rowHeight"_sv);
    float section_height = t.metric("sidebar.sectionHeight"_sv);
    float inset = t.metric("sidebar.rowInset"_sv);
    float radius = t.metric("sidebar.rowRadius"_sv);
    float icon = t.metric("sidebar.iconSize"_sv);
    float gap = t.metric("sidebar.iconGap"_sv);
    float left = t.metric("sidebar.textLeft"_sv);
    for (size_t i = 0; i < m_entries.size(); ++i) {
        auto const& entry = m_entries[i];
        float y = y_of(i);
        if (entry.section) {
            graphics.draw_text(entry.title.view(), section_font, { left + inset - 2, y + section_height - 8 }, t.color("sidebar.section"_sv));
            continue;
        }
        if (entry.row == m_selected)
            graphics.fill_rounded({ inset, y, width() - inset * 2, row_height }, radius, t.color(window() && window()->is_key() ? "sidebar.selectionActive"_sv : "sidebar.selection"_sv));
        graphics.draw_symbol(entry.symbol, { inset + left - 8, y + (row_height - icon) / 2, icon, icon }, t.color("sidebar.icon"_sv));
        float const room_for_accessory = entry.has_accessory ? icon + 6 : 0;
        graphics.draw_text_truncated(entry.title.view(), body, { inset + left - 8 + icon + gap, y + (row_height + Appearance::cap_height(body)) / 2 }, width() - inset * 2 - left - icon - gap - room_for_accessory, t.color("sidebar.text"_sv));
        if (entry.has_accessory)
            graphics.draw_symbol(entry.accessory, { width() - inset - icon - 6, y + (row_height - icon) / 2, icon, icon }, t.color("sidebar.icon"_sv));
    }
}

void Sidebar::select_row(size_t row)
{
    if (static_cast<int>(row) == m_selected)
        return;
    m_selected = static_cast<int>(row);
    set_needs_display();
    if (on_select)
        on_select(row);
}

bool Sidebar::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft)
        return false;
    float row_height = tokens().metric("sidebar.rowHeight"_sv);
    for (size_t i = 0; i < m_entries.size(); ++i) {
        auto const& entry = m_entries[i];
        float y = y_of(i);
        if (!entry.section && event.position.y >= y && event.position.y < y + row_height) {
            float const accessory = tokens().metric("sidebar.iconSize"_sv);
            if (entry.has_accessory && event.position.x >= width() - tokens().metric("sidebar.rowInset"_sv) - accessory - 8) {
                entry.accessory_action();
                return true;
            }
            select_row(static_cast<size_t>(entry.row));
            return true;
        }
    }
    return true;
}

bool Sidebar::key_down(KeyEvent const& event)
{
    if (m_rows == 0)
        return false;
    if (event.code == KeyDown) {
        select_row(m_selected < 0 ? 0 : min(static_cast<size_t>(m_selected) + 1, m_rows - 1));
        return true;
    }
    if (event.code == KeyUp) {
        select_row(m_selected <= 0 ? 0 : static_cast<size_t>(m_selected) - 1);
        return true;
    }
    return false;
}

}
