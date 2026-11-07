/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "HelpSearch.h"
#include <LibUI/Accessibility.h>
#include <LibUI/Appearance.h>
#include <LibUI/Event.h>
#include <LibUI/Graphics.h>
#include <LibUI/Platform.h>
#include <LibUI/TextField.h>
#include <LibUI/View.h>
#include <stdio.h>

namespace DesktopShell {

using namespace UI;
using Gfx::FloatRect;
using Gfx::IntPoint;
using Gfx::IntRect;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

String copy(StringView text)
{
    return MUST(String::from_view(text));
}

// Whether `text` has `part` in it, whatever the case.
bool contains_folded(StringView text, StringView part)
{
    if (part.is_empty())
        return false;
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    for (size_t at = 0; at + part.length() <= text.length(); ++at) {
        bool same = true;
        for (size_t i = 0; i < part.length() && same; ++i)
            same = lower(text[at + i]) == lower(part[i]);
        if (same)
            return true;
    }
    return false;
}

class HelpField final : public TextField {
public:
    HelpField()
        : TextField({}, FieldStyle::Rounded)
    {
        set_accessibility_label("Search"_sv);
    }

    Function<bool(KeyEvent const&)> on_key;

    bool key_down(KeyEvent const& event) override
    {
        if (on_key && on_key(event))
            return true;
        return TextField::key_down(event);
    }
};

}

class HelpView final : public View {
public:
    HelpView(HelpSearch& owner, Function<void(u32)> chosen, Function<void()> dismiss)
        : m_owner(owner)
        , m_choose(move(chosen))
        , m_dismiss(move(dismiss))
    {
        m_field = &add<HelpField>();
        m_field->on_change = [this] { find(); };
        m_field->on_key = [this](KeyEvent const& event) {
            switch (event.code) {
            case KeyEscape:
                m_dismiss();
                return true;
            case KeyDown:
                if (!m_found.is_empty())
                    m_selected = min(m_selected + 1, static_cast<int>(m_found.size()) - 1);
                set_needs_display();
                return true;
            case KeyUp:
                m_selected = max(m_selected - 1, 0);
                set_needs_display();
                return true;
            case KeyEnter:
                this->choose(m_selected);
                return true;
            default:
                return false;
            }
        };
    }

    [[nodiscard]] TextField& field() { return *m_field; }

    [[nodiscard]] float height_wanted() const
    {
        auto const& t = tokens();
        float height = t.metric("help.fieldTop"_sv) + m_field->intrinsic_size().height + t.metric("help.padding"_sv);
        if (!m_found.is_empty())
            height += t.metric("help.headingHeight"_sv) + t.metric("help.rowHeight"_sv) * static_cast<float>(m_found.size()) + t.metric("help.padding"_sv);
        return height;
    }

    Function<void()> on_size_change;

    void layout() override
    {
        auto const& t = tokens();
        float const padding = t.metric("help.padding"_sv);
        m_field->set_frame({ padding, t.metric("help.fieldTop"_sv), width() - padding * 2, m_field->intrinsic_size().height });
    }

    void draw(Graphics& graphics) override
    {
        if (m_found.is_empty())
            return;
        auto const& t = tokens();
        float const padding = t.metric("help.padding"_sv);
        float y = m_field->frame().y + m_field->frame().height + padding;
        graphics.draw_text("Menu Items"_sv, t.named_font("small"_sv), { padding, y + 13 }, t.color("secondaryLabel"_sv));
        y += t.metric("help.headingHeight"_sv);
        auto const font = t.named_font("menu"_sv);
        for (size_t i = 0; i < m_found.size(); ++i) {
            auto const& command = m_owner.commands()[m_found[i]];
            FloatRect const row { 5, y, width() - 10, t.metric("help.rowHeight"_sv) };
            bool const selected = static_cast<int>(i) == m_selected;
            if (selected)
                graphics.fill_rounded(row, t.metric("menu.selectionRadius"_sv), t.color("menu.selection"_sv));
            auto const text = selected ? t.color("menu.selectedText"_sv) : (command.enabled ? t.color("menu.text"_sv) : t.color("menu.disabled"_sv));
            float const baseline = row.y + (row.height + Appearance::cap_height(font)) / 2;
            graphics.draw_text(command.title.view(), font, { padding + 4, baseline }, text);
            // Where it is, at the right, as Help says it.
            float const path_width = Appearance::text_width(t.named_font("small"_sv), command.path.view());
            graphics.draw_text(command.path.view(), t.named_font("small"_sv), { width() - padding - path_width, baseline }, selected ? text : t.color("menu.shortcut"_sv));
            y += row.height;
        }
    }

    void mouse_moved(MouseEvent const& event) override
    {
        int const over = row_at(event.position.y);
        if (over >= 0 && over != m_selected) {
            m_selected = over;
            set_needs_display();
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        if (!FloatRect { 0, 0, width(), height() }.contains(event.position)) {
            m_dismiss();
            return true;
        }
        return false;
    }

    void mouse_up(MouseEvent const& event) override
    {
        int const index = row_at(event.position.y);
        if (index >= 0)
            choose(index);
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Menu; }
    [[nodiscard]] String accessibility_label() const override { return copy("Help"_sv); }
    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        auto const& t = tokens();
        float y = m_field->frame().y + m_field->frame().height + t.metric("help.padding"_sv) + t.metric("help.headingHeight"_sv);
        for (size_t i = 0; i < m_found.size(); ++i) {
            auto const& command = m_owner.commands()[m_found[i]];
            parts.append(Accessibility::part_of(*this, Accessibility::Role::MenuItem, command.title.view(), { 5, y, width() - 10, t.metric("help.rowHeight"_sv) }, command.path.view(), command.enabled));
            y += t.metric("help.rowHeight"_sv);
        }
    }

private:
    void find()
    {
        m_found.clear();
        auto const typed = m_field->text();
        auto const most = static_cast<size_t>(tokens().metric("help.mostResults"_sv));
        for (size_t i = 0; i < m_owner.commands().size() && m_found.size() < most; ++i) {
            if (contains_folded(m_owner.commands()[i].title.view(), typed))
                m_found.append(i);
        }
        m_selected = m_found.is_empty() ? -1 : 0;
        set_needs_display();
        if (on_size_change)
            on_size_change();
    }

    [[nodiscard]] int row_at(float y) const
    {
        auto const& t = tokens();
        float const top = m_field->frame().y + m_field->frame().height + t.metric("help.padding"_sv) + t.metric("help.headingHeight"_sv);
        if (y < top)
            return -1;
        int const index = static_cast<int>((y - top) / t.metric("help.rowHeight"_sv));
        return index < static_cast<int>(m_found.size()) ? index : -1;
    }

    void choose(int index)
    {
        if (index < 0 || static_cast<size_t>(index) >= m_found.size())
            return;
        auto const& command = m_owner.commands()[m_found[static_cast<size_t>(index)]];
        if (!command.enabled)
            return;
        u32 const id = command.id;
        auto choose = move(m_choose);
        m_dismiss();
        if (choose)
            choose(id);
    }

    HelpSearch& m_owner;
    HelpField* m_field { nullptr };
    Vector<size_t> m_found;
    int m_selected { -1 };
    Function<void(u32)> m_choose;
    Function<void()> m_dismiss;
};

void HelpSearch::set_menus(JSON::Value const& menus)
{
    m_commands.clear();
    if (!menus.is_array())
        return;
    for (size_t i = 0; i < menus.size(); ++i) {
        auto const& menu = menus.at(i);
        collect(menu, menu.get_string("title"_sv).value_or(StringView {}));
    }
}

void HelpSearch::collect(JSON::Value const& menu, StringView path)
{
    auto const* items = menu.get_array("items"_sv);
    if (!items)
        return;
    for (size_t i = 0; i < items->size(); ++i) {
        auto const& item = items->at(i);
        if (item.get_boolean("separator"_sv).value_or(false) || item.get_boolean("heading"_sv).value_or(false))
            continue;
        auto const title = item.get_string("title"_sv).value_or(StringView {});
        if (item.get_array("items"_sv)) {
            StringBuilder deeper;
            deeper.append(path);
            deeper.append(" \xE2\x96\xB8 "_sv);
            deeper.append(title);
            collect(item, deeper.view());
            continue;
        }
        Command command;
        command.title = copy(title);
        command.path = copy(path);
        command.id = static_cast<u32>(item.get_integer("id"_sv).value_or(0));
        command.enabled = item.get_boolean("enabled"_sv).value_or(true);
        m_commands.append(move(command));
    }
}

void HelpSearch::open(IntPoint at, Function<void(u32)> choose, Function<void()> closed)
{
    close();
    m_closed = move(closed);
    auto const& t = tokens();
    int const width = static_cast<int>(t.metric("help.width"_sv));
    m_window = adopt_own(*new UI::Window("Help"_sv, { at.x, at.y, width, 60 }, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
    m_window->set_transparent(true);
    m_window->backend().set_level(WS::LevelPopup);
    int const radius = static_cast<int>(t.metric("menu.radius"_sv));
    m_window->backend().set_corner_radius(radius);
    m_view = &m_window->root_view().add<HelpView>(*this, move(choose), [this] { Platform::the().defer([this] { close(); }); });
    auto resize = [this, width, radius, at] {
        int const height = static_cast<int>(m_view->height_wanted());
        m_window->set_frame({ at.x, at.y, width, height });
        m_view->set_frame({ 0, 0, static_cast<float>(width), static_cast<float>(height) });
        MaterialRegion const material { { 0, 0, width, height }, WS::Material::Popover, radius };
        m_window->set_materials(Span<MaterialRegion const>(&material, 1));
    };
    m_view->on_size_change = resize;
    resize();
    m_window->on_key_change = [this](bool key) {
        if (!key)
            Platform::the().defer([this] { close(); });
    };
    m_window->show();
    m_window->make_first_responder(&m_view->field());
    printf("help: shown\n");
    fflush(stdout);
}

void HelpSearch::close()
{
    if (!m_window)
        return;
    m_window->hide();
    auto* old = m_window.leak_ptr();
    m_view = nullptr;
    Platform::the().defer([old] { delete old; });
    if (m_closed) {
        auto closed = move(m_closed);
        closed();
    }
}

bool HelpSearch::is_open() const
{
    return m_window && m_window->is_shown();
}

}
