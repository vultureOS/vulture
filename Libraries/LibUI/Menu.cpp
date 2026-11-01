/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Menu.h>
#include <LibUI/Platform.h>

namespace UI {

using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

Menu* s_open_menu;

float ceiling(float value)
{
    return static_cast<float>(Base::Math::ceil(static_cast<double>(value)));
}

}

class Menu::KeyForwarder final : public Responder {
public:
    explicit KeyForwarder(Menu& menu)
        : m_menu(menu)
    {
    }

    bool key_down(KeyEvent const& event) override { return m_menu.key(event); }
    bool key_up(KeyEvent const&) override { return true; }

private:
    Menu& m_menu;
};

class MenuView final : public View {
public:
    explicit MenuView(Menu& menu)
        : m_menu(menu)
    {
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        auto font = t.named_font("menu"_sv);
        FloatRect all { 0, 0, width(), height() };
        graphics.fill(all, t.color("menu.tint"_sv));

        float inset = t.metric("menu.selectionInset"_sv);
        float text_left = t.metric("menu.textLeft"_sv);
        float text_right = t.metric("menu.textRight"_sv);
        float row = t.metric("menu.rowHeight"_sv);
        float check_left = t.metric("menu.checkLeft"_sv);
        float check_size = t.metric("menu.checkSize"_sv);
        float cap = Appearance::cap_height(font);
        for (size_t i = 0; i < m_menu.m_items.size(); ++i) {
            auto const& item = m_menu.m_items[i];
            auto rect = m_menu.item_rect(static_cast<int>(i));
            if (item.separator) {
                float line_y = graphics.snap(rect.y + rect.height / 2 - 0.5f);
                graphics.fill({ text_left - 11, line_y, width() - (text_left - 11) * 2, 1 }, t.color("menu.separator"_sv));
                continue;
            }
            bool highlighted = static_cast<int>(i) == m_menu.m_highlight && item.enabled;
            if (highlighted)
                graphics.fill_rounded({ inset, rect.y, width() - inset * 2, rect.height }, t.metric("menu.selectionRadius"_sv), t.color("menu.selection"_sv));
            auto text_color = t.color(!item.enabled ? "menu.disabled"_sv : (highlighted ? "menu.selectedText"_sv : "menu.text"_sv));
            float baseline = rect.y + (row + cap) / 2;
            graphics.draw_text(item.title.view(), font, { text_left, baseline }, text_color);
            if (item.checked)
                graphics.draw_symbol(Symbol::Checkmark, { check_left, rect.y + (row - check_size) / 2, check_size, check_size }, text_color, 2);
            if (item.submenu) {
                graphics.draw_symbol(Symbol::ChevronRight, { width() - text_right - check_size, rect.y + (row - check_size) / 2, check_size, check_size }, text_color, 1.8f);
            } else if (!item.shortcut_text.is_empty()) {
                auto shortcut_color = highlighted ? text_color : t.color("menu.shortcut"_sv);
                float shortcut_width = Appearance::text_width(font, item.shortcut_text.view());
                graphics.draw_text(item.shortcut_text.view(), font, { width() - text_right - shortcut_width, baseline }, shortcut_color);
            }
        }
    }

    void draw_over_children(Graphics& graphics) override
    {
        auto const& t = tokens();
        graphics.stroke_rounded({ 0, 0, width(), height() }, t.metric("menu.radius"_sv), graphics.pixel(), t.color("menu.border"_sv));
    }

    bool mouse_down(MouseEvent const& event) override
    {
        auto screen = screen_of(event);
        m_menu.pointer_at(screen, event.buttons);
        // A press outside every menu of the chain ends it.
        if (!m_menu.screen_point_in_chain(screen))
            Menu::open_menu()->close();
        return true;
    }

    void mouse_dragged(MouseEvent const& event) override
    {
        m_menu.m_dragged = true;
        m_menu.pointer_at(screen_of(event), event.buttons);
    }

    void mouse_moved(MouseEvent const& event) override
    {
        m_menu.pointer_at(screen_of(event), event.buttons);
    }

    void mouse_up(MouseEvent const& event) override
    {
        m_menu.released_at(screen_of(event));
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Menu; }

private:
    [[nodiscard]] FloatPoint screen_of(MouseEvent const& event) const
    {
        auto origin = window()->frame().origin();
        return { event.window_position.x + static_cast<float>(origin.x), event.window_position.y + static_cast<float>(origin.y) };
    }

    Menu& m_menu;
};

Menu::Menu(StringView title)
    : m_title(MUST(String::from_view(title)))
{
}

Menu::~Menu()
{
    close();
}

MenuItem& Menu::add(StringView title, Function<void()> action)
{
    MenuItem item;
    item.title = MUST(String::from_view(title));
    item.action = move(action);
    m_items.append(move(item));
    return m_items.last();
}

MenuItem& Menu::add_with_shortcut(StringView title, u16 code, StringView shown, Function<void()> action, u32 modifiers)
{
    auto& item = add(title, move(action));
    item.shortcut_code = code;
    item.shortcut_modifiers = modifiers;
    item.shortcut_text = MUST(String::from_view(shown));
    return item;
}

void Menu::add_separator()
{
    MenuItem item;
    item.separator = true;
    item.enabled = false;
    m_items.append(move(item));
}

Menu& Menu::add_submenu(StringView title)
{
    auto& item = add(title);
    item.submenu = adopt_own(*new Menu(title));
    item.submenu->m_parent = this;
    return *item.submenu;
}

FloatSize Menu::size() const
{
    auto const& t = tokens();
    auto font = t.named_font("menu"_sv);
    float row = t.metric("menu.rowHeight"_sv);
    float separator = t.metric("menu.separatorHeight"_sv);
    float padding = t.metric("menu.padding"_sv);
    float height = padding * 2;
    float widest = 0;
    float widest_shortcut = 0;
    bool has_submenu = false;
    for (auto const& item : m_items) {
        height += item.separator ? separator : row;
        if (item.separator)
            continue;
        widest = max(widest, Appearance::text_width(font, item.title.view()));
        widest_shortcut = max(widest_shortcut, Appearance::text_width(font, item.shortcut_text.view()));
        has_submenu = has_submenu || item.submenu;
    }
    float width = t.metric("menu.textLeft"_sv) + widest + t.metric("menu.textRight"_sv);
    if (widest_shortcut > 0)
        width += t.metric("menu.shortcutGap"_sv) + widest_shortcut;
    else if (has_submenu)
        width += t.metric("menu.shortcutGap"_sv);
    return { max(ceiling(width), t.metric("menu.minimumWidth"_sv)), height };
}

FloatRect Menu::item_rect(int index) const
{
    auto const& t = tokens();
    float row = t.metric("menu.rowHeight"_sv);
    float separator = t.metric("menu.separatorHeight"_sv);
    float y = t.metric("menu.padding"_sv);
    for (int i = 0; i < index; ++i)
        y += m_items[static_cast<size_t>(i)].separator ? separator : row;
    float own = m_items[static_cast<size_t>(index)].separator ? separator : row;
    return { 0, y, m_window ? m_window->size().width : size().width, own };
}

int Menu::item_at(FloatPoint point) const
{
    if (!m_window || point.x < 0 || point.x >= m_window->size().width)
        return -1;
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (item_rect(static_cast<int>(i)).contains(point))
            return static_cast<int>(i);
    }
    return -1;
}

Menu* Menu::open_menu()
{
    return s_open_menu;
}

void Menu::open(Window& owner, IntPoint screen_point, int aligned)
{
    if (s_open_menu && !m_parent)
        s_open_menu->close();
    auto const& t = tokens();
    m_owner = &owner;
    m_highlight = aligned;
    m_chosen = false;
    m_dragged = false;
    m_opened_at = Platform::the().now();
    auto wanted = size();
    int width = static_cast<int>(wanted.width);
    int height = static_cast<int>(wanted.height);
    IntPoint origin = screen_point;
    if (aligned >= 0)
        origin.y -= static_cast<int>(item_rect(aligned).y);
    // On the screen, whatever else.
    auto screen = Platform::the().screen_size();
    origin.x = clamp(origin.x, 0, max(screen.width - width, 0));
    origin.y = clamp(origin.y, 0, max(screen.height - height, 0));

    m_window = adopt_own(*new Window(m_title.view(), { origin.x, origin.y, width, height }, WS::WindowPopup));
    m_window->set_transparent(true);
    m_window->backend().set_corner_radius(static_cast<int>(t.metric("menu.radius"_sv)));
    auto& view = m_window->root_view().add<MenuView>(*this);
    view.set_frame({ 0, 0, wanted.width, wanted.height });
    m_window->set_pointer_catcher(&view);
    MaterialRegion material { { 0, 0, width, height }, WS::Material::Popover };
    m_window->set_materials(Span<MaterialRegion const>(&material, 1));
    m_window->show();
    m_window->backend().grab_pointer(true);
    if (!m_parent) {
        s_open_menu = this;
        m_key_forwarder = adopt_own(*new KeyForwarder(*this));
        owner.set_key_interceptor(m_key_forwarder.ptr());
    }
}

void Menu::close()
{
    close_submenu();
    if (!m_window)
        return;
    m_window->backend().grab_pointer(false);
    auto was = move(m_window);
    was->hide();
    if (!m_parent) {
        if (s_open_menu == this)
            s_open_menu = nullptr;
        if (m_owner)
            m_owner->set_key_interceptor(nullptr);
        m_key_forwarder = nullptr;
    }
    // (The window may be in the middle of telling us this, so it goes after.)
    auto* raw = was.leak_ptr();
    Platform::the().defer([raw] { delete raw; });
    if (!m_parent && on_close)
        on_close();
}

void Menu::close_submenu()
{
    if (m_open_child) {
        auto* child = m_open_child;
        m_open_child = nullptr;
        child->close();
    }
}

void Menu::open_submenu(int index)
{
    auto& item = m_items[static_cast<size_t>(index)];
    if (!item.submenu || m_open_child == item.submenu.ptr())
        return;
    close_submenu();
    auto rect = item_rect(index);
    auto frame = m_window->frame();
    IntPoint at { frame.right() - static_cast<int>(tokens().metric("menu.submenuOverlap"_sv)), frame.y + static_cast<int>(rect.y) - static_cast<int>(tokens().metric("menu.padding"_sv)) };
    item.submenu->m_parent = this;
    item.submenu->open(*m_owner, at);
    item.submenu->set_highlight(-1);
    m_open_child = item.submenu.ptr();
}

Menu* Menu::deepest()
{
    auto* menu = this;
    while (menu->m_open_child)
        menu = menu->m_open_child;
    return menu;
}

bool Menu::screen_point_in_chain(FloatPoint point) const
{
    auto const* root = this;
    while (root->m_parent)
        root = root->m_parent;
    for (auto const* menu = root; menu; menu = menu->m_open_child) {
        if (menu->m_window && menu->m_window->frame().to<float>().contains(point))
            return true;
    }
    return false;
}

void Menu::set_highlight(int index)
{
    if (index == m_highlight)
        return;
    m_highlight = index;
    if (m_window)
        m_window->root_view().children()[1]->set_needs_display();
}

void Menu::pointer_at(FloatPoint screen, u32)
{
    auto* root = this;
    while (root->m_parent)
        root = root->m_parent;
    // Whichever of the chain it is over, the deepest first.
    Vector<Menu*> chain;
    for (auto* menu = root; menu; menu = menu->m_open_child)
        chain.append(menu);
    for (size_t i = chain.size(); i > 0; --i) {
        auto* menu = chain[i - 1];
        if (!menu->m_window)
            continue;
        auto frame = menu->m_window->frame();
        if (!frame.to<float>().contains(screen))
            continue;
        int over = menu->item_at({ screen.x - static_cast<float>(frame.x), screen.y - static_cast<float>(frame.y) });
        if (over >= 0 && !menu->m_items[static_cast<size_t>(over)].enabled)
            over = -1;
        menu->set_highlight(over);
        if (over >= 0 && menu->m_items[static_cast<size_t>(over)].submenu)
            menu->open_submenu(over);
        else if (over >= 0)
            menu->close_submenu();
        return;
    }
    deepest()->set_highlight(-1);
}

void Menu::released_at(FloatPoint screen)
{
    auto* root = this;
    while (root->m_parent)
        root = root->m_parent;
    if (!screen_point_in_chain(screen)) {
        // Let go outside after a press that opened it: it stays until the next press.
        if (root->m_dragged)
            root->close();
        return;
    }
    for (auto* menu = root->deepest(); menu; menu = menu->m_parent) {
        if (!menu->m_window)
            continue;
        auto frame = menu->m_window->frame();
        if (!frame.to<float>().contains(screen))
            continue;
        int over = menu->item_at({ screen.x - static_cast<float>(frame.x), screen.y - static_cast<float>(frame.y) });
        // The release of the press that opened it, right after, chooses nothing.
        double since = Platform::the().now() - root->m_opened_at;
        if (over >= 0 && (root->m_dragged || since > 0.3))
            menu->choose(over);
        return;
    }
}

void Menu::choose(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= m_items.size())
        return;
    auto& item = m_items[static_cast<size_t>(index)];
    if (!item.enabled || item.separator || item.submenu)
        return;
    auto* root = this;
    while (root->m_parent)
        root = root->m_parent;
    // Closing first: the action may open another.
    auto action = move(item.action);
    root->close();
    if (action) {
        action();
        item.action = move(action);
    }
}

bool Menu::key(KeyEvent const& event)
{
    auto* menu = deepest();
    auto next_enabled = [&](int from, int by) {
        int count = static_cast<int>(menu->m_items.size());
        int at = from;
        for (int i = 0; i < count; ++i) {
            at += by;
            if (at < 0)
                at = count - 1;
            if (at >= count)
                at = 0;
            if (menu->m_items[static_cast<size_t>(at)].enabled)
                return at;
        }
        return from;
    };
    switch (event.code) {
    case KeyDown:
        menu->set_highlight(next_enabled(menu->m_highlight < 0 ? -1 : menu->m_highlight, 1));
        return true;
    case KeyUp:
        menu->set_highlight(next_enabled(menu->m_highlight < 0 ? 0 : menu->m_highlight, -1));
        return true;
    case KeyRight:
        if (menu->m_highlight >= 0 && menu->m_items[static_cast<size_t>(menu->m_highlight)].submenu) {
            menu->open_submenu(menu->m_highlight);
            menu->m_open_child->set_highlight(next_enabled(-1, 1));
        }
        return true;
    case KeyLeft:
        if (menu->m_parent)
            menu->m_parent->close_submenu();
        return true;
    case KeyEnter:
    case KeySpace:
        menu->choose(menu->m_highlight);
        return true;
    case KeyEscape:
        close();
        return true;
    default:
        return true;
    }
}

bool Menu::perform_shortcut(KeyEvent const& event)
{
    for (auto& item : m_items) {
        if (item.submenu && item.submenu->perform_shortcut(event))
            return true;
        if (item.shortcut_code == event.code && item.enabled && (event.modifiers & 0xF) == item.shortcut_modifiers) {
            if (item.action)
                item.action();
            return true;
        }
    }
    return false;
}

// ---- Pop-up button

PopUpButton::PopUpButton(bool pulls_down)
    : m_menu(adopt_own(*new Menu))
    , m_pulls_down(pulls_down)
{
}

PopUpButton::~PopUpButton() = default;

void PopUpButton::add_item(StringView title)
{
    int index = static_cast<int>(m_menu->items().size());
    m_menu->add(title, [this, index] {
        if (!m_pulls_down)
            set_selected_index(index);
        if (on_change)
            on_change(index);
        if (on_action)
            on_action();
    });
    if (m_selected < 0 && !m_pulls_down)
        set_selected_index(index);
    if (parent())
        parent()->set_needs_layout();
}

void PopUpButton::add_separator()
{
    m_menu->add_separator();
}

void PopUpButton::set_selected_index(int index)
{
    if (index == m_selected)
        return;
    if (!m_pulls_down) {
        for (size_t i = 0; i < m_menu->items().size(); ++i)
            m_menu->items()[i].checked = static_cast<int>(i) == index;
    }
    m_selected = index;
    set_needs_display();
}

StringView PopUpButton::selected_title() const
{
    if (m_pulls_down)
        return m_title.view();
    if (m_selected < 0 || static_cast<size_t>(m_selected) >= m_menu->items().size())
        return {};
    return m_menu->items()[static_cast<size_t>(m_selected)].title.view();
}

void PopUpButton::set_title(StringView title)
{
    m_title = MUST(String::from_view(title));
    if (parent())
        parent()->set_needs_layout();
    set_needs_display();
}

FloatSize PopUpButton::intrinsic_size() const
{
    auto const& t = tokens();
    auto size = control_size();
    auto font = t.bezel_font(size);
    float widest = 0;
    if (m_pulls_down) {
        widest = Appearance::text_width(font, m_title.view());
    } else {
        for (auto const& item : m_menu->items()) {
            if (!item.separator)
                widest = max(widest, Appearance::text_width(font, item.title.view()));
        }
    }
    return { ceiling(widest + t.metric("popup"_sv, size, "extra"_sv)), t.metric("popup"_sv, size, "height"_sv) };
}

float PopUpButton::baseline() const
{
    return tokens().metric("popup"_sv, control_size(), "baseline"_sv);
}

float PopUpButton::focus_ring_radius() const
{
    return tokens().metric("popup"_sv, control_size(), "radius"_sv);
}

void PopUpButton::draw(Graphics& graphics)
{
    auto const& t = tokens();
    auto size = control_size();
    FloatRect shape { 0, 0, width(), height() };
    bool enabled = is_enabled();
    bool active = window() && window()->is_key();
    bool open = m_menu->is_open();
    graphics.draw_bezel(shape, t.metric("popup"_sv, size, "radius"_sv), BezelPaint::from_tokens(!enabled ? "bezelDisabled"_sv : ((is_pressed() || open) ? "bezelPressed"_sv : "bezel"_sv)));
    auto font = t.bezel_font(size);
    graphics.draw_text(selected_title(), font, { t.metric("popup"_sv, size, "paddingLeft"_sv), t.metric("popup"_sv, size, "baseline"_sv) }, t.color(enabled ? "controlText"_sv : "controlTextDisabled"_sv));

    float arrow = t.metric("popup"_sv, size, "arrowSize"_sv);
    float inset = t.metric("popup"_sv, size, "arrowInset"_sv);
    FloatRect box { width() - inset - arrow, (height() - arrow) / 2, arrow, arrow };
    auto group = "popupArrow"_sv;
    if (enabled && active) {
        graphics.fill_vertical_gradient(box, t.metric("popup"_sv, size, "arrowRadius"_sv), t.color("popupArrow.fillTop"_sv), t.color("popupArrow.fillBottom"_sv));
        graphics.draw_symbol(m_pulls_down ? Symbol::ChevronDown : Symbol::ChevronUpDown, box.inflated(-arrow / 8), t.color("popupArrow.symbol"_sv), m_pulls_down ? 2.6f : 2.2f);
    } else {
        graphics.draw_symbol(m_pulls_down ? Symbol::ChevronDown : Symbol::ChevronUpDown, box.inflated(-arrow / 8), t.color(enabled ? "controlSymbol"_sv : "controlTextDisabled"_sv), m_pulls_down ? 2.6f : 2.2f);
    }
    (void)group;
}

void PopUpButton::open_menu()
{
    if (!window() || m_menu->is_open())
        return;
    auto origin = window()->frame().origin();
    auto corner = to_window({ 0, 0 });
    float text_left = tokens().metric("menu.textLeft"_sv) - tokens().metric("popup"_sv, control_size(), "paddingLeft"_sv);
    IntPoint at { origin.x + static_cast<int>(corner.x - text_left), origin.y + static_cast<int>(corner.y) };
    if (m_pulls_down)
        at.y += static_cast<int>(height());
    m_menu->on_close = [this] { set_needs_display(); };
    m_menu->open(*window(), at, m_pulls_down ? -1 : m_selected);
    set_needs_display();
}

bool PopUpButton::mouse_down(MouseEvent const& event)
{
    if (event.button != WS::ButtonLeft)
        return false;
    open_menu();
    // The menu has the pointer now: it ends this press.
    return false;
}

bool PopUpButton::key_down(KeyEvent const& event)
{
    if (event.code == KeySpace || event.code == KeyDown || event.code == KeyUp) {
        open_menu();
        return true;
    }
    return false;
}

String PopUpButton::accessibility_value() const
{
    return MUST(String::from_view(selected_title()));
}

bool PopUpButton::accessibility_press()
{
    open_menu();
    return true;
}

// ---- Combo box

ComboBox::ComboBox()
    : TextField({}, FieldStyle::Plain)
    , m_menu(adopt_own(*new Menu))
{
}

void ComboBox::add_item(StringView title)
{
    auto copy = MUST(String::from_view(title));
    m_menu->add(title, [this, copy = move(copy)] {
        set_text(copy.view());
        if (on_change)
            on_change();
        if (on_commit)
            on_commit();
    });
}

FloatSize ComboBox::intrinsic_size() const
{
    return { -1, tokens().metric("combo"_sv, control_size(), "height"_sv) };
}

float ComboBox::text_baseline() const
{
    return tokens().metric("combo"_sv, control_size(), "baseline"_sv);
}

float ComboBox::baseline() const
{
    return text_baseline();
}

float ComboBox::text_left() const
{
    return tokens().metric("combo"_sv, control_size(), "padding"_sv);
}

float ComboBox::text_right_margin() const
{
    return tokens().metric("combo"_sv, control_size(), "arrow"_sv) + 4;
}

float ComboBox::focus_ring_radius() const
{
    return tokens().metric("combo"_sv, control_size(), "radius"_sv);
}

FloatRect ComboBox::arrow_rect() const
{
    float arrow = tokens().metric("combo"_sv, control_size(), "arrow"_sv);
    return { width() - arrow - 2, (height() - arrow) / 2, arrow, arrow };
}

void ComboBox::draw_face(Graphics& graphics)
{
    graphics.draw_bezel({ 0, 0, width(), height() }, tokens().metric("combo"_sv, control_size(), "radius"_sv), BezelPaint::from_tokens(is_enabled() ? "comboFace"_sv : "comboFaceDisabled"_sv));
}

void ComboBox::draw_decorations(Graphics& graphics)
{
    auto const& t = tokens();
    auto box = arrow_rect();
    graphics.fill_vertical_gradient(box, t.metric("popup"_sv, control_size(), "arrowRadius"_sv), t.color("popupArrow.fillTop"_sv), t.color("popupArrow.fillBottom"_sv));
    graphics.draw_symbol(Symbol::ChevronDown, box.inflated(-box.width / 8), t.color("popupArrow.symbol"_sv), 2.6f);
}

bool ComboBox::mouse_down(MouseEvent const& event)
{
    if (arrow_rect().contains(event.position)) {
        auto origin = window()->frame().origin();
        auto corner = to_window({ 0, 0 });
        m_menu->open(*window(), { origin.x + static_cast<int>(corner.x), origin.y + static_cast<int>(corner.y + height()) });
        return false;
    }
    return TextField::mouse_down(event);
}

}
