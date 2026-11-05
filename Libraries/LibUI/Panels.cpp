/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibUI/Motion.h>
#include <LibUI/Panels.h>
#include <LibUI/Platform.h>

namespace UI {

void draw_sheet_face(Graphics& graphics, Gfx::FloatRect shape)
{
    auto const& t = Appearance::the();
    float const radius = t.metric("sheet.radius"_sv);
    // Its shadow, in stacked rings, softly: nothing under it needs to be read.
    for (int i = 0; i < 12; ++i) {
        float const grown = static_cast<float>(12 - i) * 2;
        graphics.fill_rounded({ shape.x - grown, shape.y - grown / 2, shape.width + grown * 2, shape.height + grown * 1.5f }, radius + grown, Gfx::Color { 0, 0, 0, 3 });
    }
    graphics.fill_rounded(shape, radius, t.color("sheet.fill"_sv));
    // On a dark window the shadow doesn't show, and it would have no edge at all.
    graphics.stroke_rounded(shape, radius, 1, t.color("sheet.edge"_sv));
}

using namespace Gfx;
namespace Math = Base::Math;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

}

// ---- Sheets

class SheetScrim final : public View {
public:
    SheetScrim(NonnullOwnPtr<View> content, FloatSize size, Function<void()> on_dismissed)
        : m_size(size)
        , m_on_dismissed(move(on_dismissed))
    {
        m_holder = &add<SheetFace>();
        m_content = content.ptr();
        m_holder->adopt_child(move(content));
    }

    void layout() override
    {
        auto const& t = tokens();
        float x = (width() - m_size.width) / 2;
        float y = t.metric("sheet.topMargin"_sv);
        m_holder->set_frame({ x, y, m_size.width, m_size.height });
        m_content->set_frame({ 0, 0, m_size.width, m_size.height });
    }

    void draw(Graphics& graphics) override
    {
        graphics.fill({ 0, 0, width(), height() }, tokens().color("sheet.scrim"_sv));
    }

    // All clicks stop here: what is under waits.
    bool mouse_down(MouseEvent const&) override { return true; }
    bool scroll_wheel(ScrollEvent const&) override { return true; }

    void slide_in()
    {
        auto const distance = m_size.height + 12;
        m_holder->animate_translation({ 0, -distance }, { 0, 0 }, Animation::Timing::spring_with(tokens().metric("animation.standard"_sv) * 1.3f, 0), [this] { m_holder->set_wants_layer(false); });
    }

    void run_dismissed()
    {
        auto callback = move(m_on_dismissed);
        if (callback)
            callback();
    }

    [[nodiscard]] View& holder() { return *m_holder; }
    [[nodiscard]] FloatSize content_size() const { return m_size; }

private:
    class SheetFace final : public View {
    public:
        SheetFace() { set_clips_children(false); }

        [[nodiscard]] Insets overflow() const override { return { 2, 24, 40, 24 }; }

        void draw(Graphics& graphics) override
        {
            draw_sheet_face(graphics, { 0, 0, width(), height() });
        }

        [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Sheet; }
    };

    View* m_holder { nullptr };
    View* m_content { nullptr };
    FloatSize m_size;
    Function<void()> m_on_dismissed;
};

namespace {

struct PresentedSheet {
    Window* window;
    SheetScrim* scrim;
    Responder* previous_responder;
};

Vector<PresentedSheet> s_sheets;

PresentedSheet* sheet_of(Window& window)
{
    for (auto& sheet : s_sheets) {
        if (sheet.window == &window)
            return &sheet;
    }
    return nullptr;
}

}

void Sheet::present(Window& window, NonnullOwnPtr<View> content, FloatSize size, Function<void()> on_dismissed)
{
    VERIFY(!sheet_of(window));
    auto* previous = window.first_responder();
    auto& scrim = window.root_view().add<SheetScrim>(move(content), size, move(on_dismissed));
    float top = window.sheet_top();
    scrim.set_frame({ 0, top, window.size().width, window.size().height - top });
    s_sheets.append(PresentedSheet { &window, &scrim, previous });
    window.make_first_responder(nullptr);
    window.set_focus_scope(&scrim);
    window.update_layout_now();
    scrim.slide_in();
    window.focus_next(false);
}

void Sheet::dismiss(Window& window)
{
    auto* sheet = sheet_of(window);
    if (!sheet)
        return;
    auto* scrim = sheet->scrim;
    auto* previous = sheet->previous_responder;
    s_sheets.remove_all_matching([&](auto const& other) { return other.window == &window; });
    window.set_focus_scope(nullptr);
    window.make_first_responder(previous);
    window.set_default_action(nullptr);
    window.set_cancel_action(nullptr);
    auto const distance = scrim->content_size().height + 12;
    auto finish = [scrim, &window] {
        scrim->run_dismissed();
        window.root_view().remove_child(*scrim);
    };
    scrim->holder().animate_translation({ 0, 0 }, { 0, -distance }, Animation::Timing::ease_in(tokens().metric("animation.standard"_sv) * 0.8f), move(finish));
}

bool Sheet::is_presented(Window& window)
{
    return sheet_of(window) != nullptr;
}

// ---- Alert

namespace {

// Whichever button goes first ends it.
class SharedChoice final : public RefCounted<SharedChoice> {
public:
    SharedChoice(Window& window, Function<void(size_t)> then)
        : m_window(window)
        , m_then(move(then))
    {
    }

    void choose(size_t index)
    {
        auto then = move(m_then);
        Sheet::dismiss(m_window);
        if (then)
            then(index);
    }

private:
    Window& m_window;
    Function<void(size_t)> m_then;
};

}

Alert::Alert(StringView message, StringView information)
    : m_message(MUST(String::from_view(message)))
    , m_information(MUST(String::from_view(information)))
{
}

void Alert::add_button(StringView title, bool is_default)
{
    m_buttons.append(ButtonDefinition { MUST(String::from_view(title)), is_default || m_buttons.is_empty() });
    if (is_default) {
        for (size_t i = 0; i + 1 < m_buttons.size(); ++i)
            m_buttons[i].is_default = false;
    }
}

void Alert::set_symbol(Symbol symbol)
{
    m_symbol = symbol;
}

namespace {

class AlertIcon final : public View {
public:
    explicit AlertIcon(Symbol symbol)
        : m_symbol(symbol)
    {
    }

    [[nodiscard]] FloatSize intrinsic_size() const override
    {
        float size = tokens().metric("alert.iconSize"_sv);
        return { size, size };
    }

    void draw(Graphics& graphics) override
    {
        graphics.draw_symbol(m_symbol, { 0, 0, width(), height() }, tokens().color("accent"_sv), 1.6f);
    }

private:
    Symbol m_symbol;
};

}

void Alert::show(Window& window, Function<void(size_t)> then)
{
    Platform::the().play_sound("alert"_sv);
    auto const& t = tokens();
    float padding = t.metric("alert.padding"_sv);
    float alert_width = t.metric("alert.width"_sv);
    auto* content = new StackView(Axis::Vertical, t.metric("alert.spacing"_sv), Alignment::Center, Insets { padding, padding, padding, padding });
    (void)content->add<AlertIcon>(m_symbol);
    auto& message = content->add<Label>(m_message.view(), t.named_font("headline"_sv));
    message.set_alignment(TextAlignment::Center);
    message.set_wraps(true);
    message.set_fixed_width(alert_width - padding * 2);
    float total = padding * 2 + t.metric("alert.iconSize"_sv);
    float line = Appearance::line_height(t.named_font("headline"_sv));
    message.set_frame({ 0, 0, alert_width - padding * 2, line });
    float message_height = message.intrinsic_size().height;
    message.set_fixed_height(message_height);
    total += message_height + t.metric("alert.spacing"_sv);
    if (!m_information.is_empty()) {
        auto& information = content->add<Label>(m_information.view(), t.named_font("small"_sv));
        information.set_alignment(TextAlignment::Center);
        information.set_wraps(true);
        information.set_fixed_width(alert_width - padding * 2);
        information.set_frame({ 0, 0, alert_width - padding * 2, line });
        float information_height = information.intrinsic_size().height;
        information.set_fixed_height(information_height);
        total += information_height + t.metric("alert.spacing"_sv);
    }

    NonnullRefPtr<SharedChoice> choose_shared = adopt_ref(*new SharedChoice(window, move(then)));
    window.set_cancel_action(nullptr);
    float buttons_height = 0;
    size_t count = m_buttons.size();
    float button_height = t.metric("alert.buttonHeight"_sv);
    float spacing = t.metric("alert.buttonSpacing"_sv);
    float button_width = alert_width - padding * 2;

    // Two sit side by side while both titles fit half the width, the default on the right; otherwise, and with
    // more, they're stacked with the default first. The order they were added in is what the answer means.
    auto fits_half_width = [&] {
        for (auto const& definition : m_buttons) {
            Button measure(definition.title.view(), definition.is_default ? ButtonStyle::Default : ButtonStyle::Push);
            measure.set_control_size(ControlSize::Large);
            if (measure.intrinsic_size().width > (button_width - spacing) / 2)
                return false;
        }
        return true;
    };
    bool side_by_side = count == 2 && fits_half_width();
    size_t default_index = 0;
    for (size_t i = 0; i < count; ++i) {
        if (m_buttons[i].is_default)
            default_index = i;
    }
    Vector<size_t> shown;
    if (!side_by_side)
        shown.append(default_index);
    for (size_t i = 0; i < count; ++i) {
        if (i != default_index)
            shown.append(i);
    }
    if (side_by_side)
        shown.append(default_index);

    auto* row = side_by_side ? &content->add<StackView>(Axis::Horizontal, spacing) : nullptr;
    if (row) {
        row->stack().set_distribution(Distribution::Equal);
        row->set_fixed_size({ button_width, button_height });
        buttons_height = button_height;
    }
    for (size_t place = 0; place < shown.size(); ++place) {
        size_t i = shown[place];
        auto const& definition = m_buttons[i];
        auto* parent_view = row ? static_cast<View*>(row) : static_cast<View*>(content);
        auto& button = parent_view->add<Button>(definition.title.view(), definition.is_default ? ButtonStyle::Default : ButtonStyle::Push);
        button.set_control_size(ControlSize::Large);
        if (!row) {
            button.set_fixed_width(button_width);
            buttons_height += button_height + (place + 1 < count ? spacing : 0);
        }
        button.on_action = [choose_shared, i] { choose_shared->choose(i); };
        if (definition.is_default)
            window.set_default_action([choose_shared, i] { choose_shared->choose(i); });
        // Escape means no: only ever the button that says so. It mustn't be one that loses something.
        if (definition.title.view() == "Cancel"_sv)
            window.set_cancel_action([choose_shared, i] { choose_shared->choose(i); });
    }
    total += buttons_height;
    Sheet::present(window, adopt_own(*static_cast<View*>(content)), { alert_width, total });
    // The default went with whatever was there before; these are the sheet's.
}

// ---- Popover

class Popover::Frame final : public View {
public:
    Frame(Popover& popover, ArrowEdge edge)
        : m_popover(popover)
        , m_edge(edge)
    {
    }

    void set_arrow_x(float x) { m_arrow_x = x; }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float arrow = t.metric("popover.arrow"_sv);
        float radius = t.metric("popover.radius"_sv);
        float shadow = t.metric("popover.shadow"_sv);
        // Inside the shadow's margin, and the arrow's room.
        FloatRect body { shadow, shadow + (m_edge == ArrowEdge::Top ? arrow : 0), width() - shadow * 2, height() - shadow * 2 - arrow };
        for (int i = static_cast<int>(shadow); i > 0; i -= 2) {
            auto spread = static_cast<float>(i);
            graphics.fill_rounded({ body.x - spread, body.y - spread * 0.5f, body.width + spread * 2, body.height + spread * 1.5f }, radius + spread, Color { 0, 0, 0, 5 });
        }
        Path path;
        path.add_rounded_rect(body, radius);
        float tip_y = m_edge == ArrowEdge::Top ? shadow : height() - shadow;
        float base_y = m_edge == ArrowEdge::Top ? shadow + arrow + 0.5f : height() - shadow - arrow - 0.5f;
        path.move_to({ m_arrow_x - arrow, base_y });
        path.line_to({ m_arrow_x, tip_y });
        path.line_to({ m_arrow_x + arrow, base_y });
        path.close();
        graphics.painter().fill_path(path, t.color("popover.fill"_sv));
    }

    bool mouse_down(MouseEvent const& event) override
    {
        auto origin = window()->frame().origin();
        FloatPoint screen { event.window_position.x + static_cast<float>(origin.x), event.window_position.y + static_cast<float>(origin.y) };
        if (!window()->frame().to<float>().contains(screen))
            m_popover.close();
        return true;
    }

private:
    Popover& m_popover;
    ArrowEdge m_edge;
    float m_arrow_x { 0 };
};

class Popover::KeyForwarder final : public Responder {
public:
    explicit KeyForwarder(Popover& popover)
        : m_popover(popover)
    {
    }

    bool key_down(KeyEvent const& event) override
    {
        if (event.code == KeyEscape) {
            m_popover.close();
            return true;
        }
        if (auto* window = m_popover.window())
            window->handle_key(true, event.code, event.modifiers, event.text);
        return true;
    }

    bool key_up(KeyEvent const& event) override
    {
        if (auto* window = m_popover.window())
            window->handle_key(false, event.code, event.modifiers, event.text);
        return true;
    }

private:
    Popover& m_popover;
};

Popover::Popover() = default;

Popover::~Popover()
{
    close();
}

void Popover::show(Window& owner, NonnullOwnPtr<View> content, FloatSize size, FloatRect anchor, ArrowEdge edge)
{
    close();
    auto const& t = tokens();
    m_owner = &owner;
    float arrow = t.metric("popover.arrow"_sv);
    float shadow = t.metric("popover.shadow"_sv);
    auto origin = owner.frame().origin();
    float anchor_x = static_cast<float>(origin.x) + anchor.x + anchor.width / 2;
    float top = edge == ArrowEdge::Top ? static_cast<float>(origin.y) + anchor.bottom() : static_cast<float>(origin.y) + anchor.y - size.height - arrow;
    float left = anchor_x - size.width / 2;
    auto screen = Platform::the().screen_size();
    left = clamp(left, 4.0f, max(static_cast<float>(screen.width) - size.width - 4, 4.0f));
    // With room for its arrow and its own shadow: an odd shape, which a window's
    // corners and shadow cannot be. So it has neither, and no material.
    IntRect frame { static_cast<int>(left - shadow), static_cast<int>(top - shadow), static_cast<int>(size.width + shadow * 2), static_cast<int>(size.height + arrow + shadow * 2) };
    m_window = adopt_own(*new Window("popover"_sv, frame, WS::WindowPopup | WS::WindowBare));
    m_window->set_transparent(true);
    auto& view = m_window->root_view().add<Frame>(*this, edge);
    view.set_frame({ 0, 0, static_cast<float>(frame.width), static_cast<float>(frame.height) });
    m_window->set_pointer_catcher(&view);
    view.set_arrow_x(anchor_x - left + shadow);
    auto* raw = content.ptr();
    view.adopt_child(move(content));
    raw->set_frame({ shadow, shadow + (edge == ArrowEdge::Top ? arrow : 0), size.width, size.height });
    m_window->show();
    m_window->backend().grab_pointer(true);
    m_forwarder = adopt_own(*new KeyForwarder(*this));
    owner.set_key_interceptor(m_forwarder.ptr());
    m_window->handle_activation(true);
}

void Popover::close()
{
    if (!m_window)
        return;
    m_window->backend().grab_pointer(false);
    if (m_owner)
        m_owner->set_key_interceptor(nullptr);
    m_forwarder = nullptr;
    auto* raw = m_window.leak_ptr();
    raw->hide();
    Platform::the().defer([raw] { delete raw; });
    if (on_close)
        on_close();
}

// ---- File panel

namespace {

struct FileEntry {
    String name;
    bool is_directory;
};

String joined(StringView directory, StringView name)
{
    StringBuilder builder;
    builder.append(directory);
    if (!directory.ends_with('/'))
        builder.append("/"_sv);
    builder.append(name);
    return MUST(builder.to_string());
}

class FileList final : public TableModel {
public:
    void load(StringView directory)
    {
        m_directory = MUST(String::from_view(directory));
        m_entries.clear();
        auto listed = Platform::the().list_directory(directory);
        if (listed.is_error())
            return;
        for (auto const& entry : listed.value())
            m_entries.append(FileEntry { MUST(String::from_view(entry.name.view())), entry.is_directory });
        // Folders and files together, by name, as Files lists them.
        for (size_t i = 1; i < m_entries.size(); ++i) {
            for (size_t j = i; j > 0 && compare(m_entries[j - 1].name.view(), m_entries[j].name.view()) > 0; --j) {
                auto moved = move(m_entries[j]);
                m_entries[j] = move(m_entries[j - 1]);
                m_entries[j - 1] = move(moved);
            }
        }
    }

    [[nodiscard]] size_t row_count() const override { return m_entries.size(); }
    [[nodiscard]] String text_at(size_t row, size_t) const override { return MUST(String::from_view(m_entries[row].name.view())); }
    [[nodiscard]] bool has_symbol_at(size_t) const override { return true; }
    [[nodiscard]] Symbol symbol_at(size_t row) const override { return m_entries[row].is_directory ? Symbol::Folder : Symbol::File; }
    [[nodiscard]] FileEntry const& entry(size_t row) const { return m_entries[row]; }
    [[nodiscard]] StringView directory() const { return m_directory.view(); }

private:
    static int compare(StringView a, StringView b)
    {
        for (size_t i = 0; i < a.length() && i < b.length(); ++i) {
            auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
            char x = lower(a[i]), y = lower(b[i]);
            if (x != y)
                return x < y ? -1 : 1;
        }
        return a.length() < b.length() ? -1 : (a.length() > b.length() ? 1 : 0);
    }

    String m_directory;
    Vector<FileEntry> m_entries;
};

class FilePanelView final : public StackView {
public:
    FilePanelView(Window& window, FilePanel::Kind kind, StringView directory, StringView suggested, Function<void(StringView)> then)
        : StackView(Axis::Vertical, 8, Alignment::Fill, Insets::all(tokens().metric("alert.padding"_sv)))
        , m_window(window)
        , m_kind(kind)
        , m_then(move(then))
    {
        auto* path_row = &add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        m_up = &path_row->add<Button>(StringView {}, ButtonStyle::Borderless);
        m_up->set_symbol(Symbol::ChevronLeft);
        m_up->set_fixed_size({ 24, 24 });
        m_up->on_action = [this] { go_up(); };
        m_path = &path_row->add<Label>(directory);
        m_path->set_stretch(1, 0);
        path_row->set_fixed_height(24);

        if (kind == FilePanel::Kind::Save) {
            auto* name_row = &add<StackView>(Axis::Horizontal, 8, Alignment::Center);
            (void)name_row->add<Label>("Save As:"_sv);
            m_name = &name_row->add<TextField>(suggested);
            m_name->set_stretch(1, 0);
            name_row->set_fixed_height(22);
        }

        m_table = &add<TableView>(&m_files);
        m_table->set_headers_visible(false);
        m_table->add_column("Name"_sv, tokens().metric("panel.width"_sv) - tokens().metric("alert.padding"_sv) * 2 - tokens().metric("table.selectionInset"_sv) * 2);
        m_table->set_stretch(0, 1);
        m_table->on_activate = [this](size_t row) { activate(row); };
        m_table->on_selection_change = [this] { selection_changed(); };

        auto* buttons = &add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& filler = buttons->add<View>();
        filler.set_stretch(1, 0);
        auto& cancel = buttons->add<Button>("Cancel"_sv);
        cancel.on_action = [this] { finish({}); };
        m_confirm = &buttons->add<Button>(kind == FilePanel::Kind::Save ? "Save"_sv : "Open"_sv, ButtonStyle::Default);
        m_confirm->on_action = [this] { confirm(); };
        buttons->set_fixed_height(20);
        load(directory);
        window.set_default_action([this] { confirm(); });
        window.set_cancel_action([this] { finish({}); });
    }

private:
    void load(StringView directory)
    {
        m_files.load(directory);
        m_path->set_text(directory);
        m_table->clear_selection();
        m_table->reload();
        m_up->set_enabled(directory != "/"_sv);
        selection_changed();
    }

    void selection_changed()
    {
        if (m_kind == FilePanel::Kind::Open) {
            m_confirm->set_enabled(m_table->selected_row() >= 0);
            return;
        }
        if (m_kind == FilePanel::Kind::OpenFolder) {
            int selected = m_table->selected_row();
            m_confirm->set_enabled(selected < 0 || m_files.entry(static_cast<size_t>(selected)).is_directory);
            return;
        }
        int row = m_table->selected_row();
        if (row >= 0 && !m_files.entry(static_cast<size_t>(row)).is_directory)
            m_name->set_text(m_files.entry(static_cast<size_t>(row)).name.view());
    }

    void go_up()
    {
        auto directory = m_files.directory();
        size_t end = directory.length();
        while (end > 1 && directory[end - 1] != '/')
            --end;
        if (end > 1)
            --end;
        auto parent_directory = MUST(String::from_view(directory.substring_view(0, end)));
        load(parent_directory.view());
    }

    void activate(size_t row)
    {
        auto const& entry = m_files.entry(row);
        auto path = joined(m_files.directory(), entry.name.view());
        if (entry.is_directory) {
            load(path.view());
            return;
        }
        if (m_kind == FilePanel::Kind::Open)
            finish(path.view());
    }

    void confirm()
    {
        if (m_kind == FilePanel::Kind::Save) {
            if (m_name->text().is_empty())
                return;
            finish(joined(m_files.directory(), m_name->text()).view());
            return;
        }
        int row = m_table->selected_row();
        if (m_kind == FilePanel::Kind::OpenFolder) {
            if (row < 0) {
                auto directory = MUST(String::from_view(m_files.directory()));
                finish(directory.view());
            } else if (m_files.entry(static_cast<size_t>(row)).is_directory) {
                finish(joined(m_files.directory(), m_files.entry(static_cast<size_t>(row)).name.view()).view());
            }
            return;
        }
        if (row >= 0)
            activate(static_cast<size_t>(row));
    }

    void finish(StringView path)
    {
        auto copy = MUST(String::from_view(path));
        auto then = move(m_then);
        auto& window = m_window;
        Sheet::dismiss(window);
        if (then)
            then(copy.view());
    }

    Window& m_window;
    FilePanel::Kind m_kind;
    Function<void(StringView)> m_then;
    FileList m_files;
    TableView* m_table { nullptr };
    Label* m_path { nullptr };
    TextField* m_name { nullptr };
    Button* m_up { nullptr };
    Button* m_confirm { nullptr };
};

}

void FilePanel::run(Window& window, Kind kind, StringView directory, StringView suggested_name, Function<void(StringView)> then)
{
    auto const& t = tokens();
    auto* view = new FilePanelView(window, kind, directory, suggested_name, move(then));
    Sheet::present(window, adopt_own(*static_cast<View*>(view)), { t.metric("panel.width"_sv), t.metric("panel.height"_sv) });
}

// ---- Color panel

namespace {

// Hue around, saturation out from the middle.
class ColorWheel final : public View {
public:
    Function<void(HSV)> on_change;

    void set_hsv(HSV hsv)
    {
        bool same_value = hsv.value == m_hsv.value;
        m_hsv = hsv;
        if (!same_value)
            m_picture = nullptr;
        set_needs_display();
    }

    void draw(Graphics& graphics) override
    {
        float diameter = min(width(), height());
        float radius = diameter / 2;
        float scale = graphics.scale();
        int pixels = static_cast<int>(diameter * scale);
        if (!m_picture || m_picture->width() != pixels) {
            m_picture = MUST(Bitmap::create({ pixels, pixels }));
            float center = static_cast<float>(pixels) / 2;
            for (int y = 0; y < pixels; ++y) {
                for (int x = 0; x < pixels; ++x) {
                    float dx = (static_cast<float>(x) + 0.5f - center) / center;
                    float dy = (static_cast<float>(y) + 0.5f - center) / center;
                    float distance = static_cast<float>(Math::square_root(static_cast<double>(dx * dx + dy * dy)));
                    if (distance > 1 + 1 / center)
                        continue;
                    float angle = static_cast<float>(Math::arc_tangent2(static_cast<double>(dy), static_cast<double>(dx))) / (2 * static_cast<float>(Math::pi));
                    if (angle < 0)
                        angle += 1;
                    float edge = clamp((1 - distance) * center + 0.5f, 0.0f, 1.0f);
                    m_picture->set_pixel_at(x, y, color_from_hsv({ angle, min(distance, 1.0f), m_hsv.value }, static_cast<u8>(edge * 255)).to_pixel());
                }
            }
        }
        graphics.painter().draw_bitmap(*m_picture, FloatRect { 0, 0, diameter, diameter });
        float angle = m_hsv.hue * 2 * static_cast<float>(Math::pi);
        FloatPoint at { radius + static_cast<float>(Math::cosine(static_cast<double>(angle))) * m_hsv.saturation * radius,
            radius + static_cast<float>(Math::sine(static_cast<double>(angle))) * m_hsv.saturation * radius };
        graphics.fill_circle(at, 5, Color { 255, 255, 255, 255 });
        graphics.fill_circle(at, 4, color_from_hsv(m_hsv));
    }

    bool mouse_down(MouseEvent const& event) override
    {
        pick(event.position);
        return true;
    }

    void mouse_dragged(MouseEvent const& event) override { pick(event.position); }

private:
    void pick(FloatPoint point)
    {
        float radius = min(width(), height()) / 2;
        float dx = point.x - radius;
        float dy = point.y - radius;
        float distance = static_cast<float>(Math::square_root(static_cast<double>(dx * dx + dy * dy))) / radius;
        float angle = static_cast<float>(Math::arc_tangent2(static_cast<double>(dy), static_cast<double>(dx))) / (2 * static_cast<float>(Math::pi));
        if (angle < 0)
            angle += 1;
        m_hsv.hue = angle;
        m_hsv.saturation = min(distance, 1.0f);
        set_needs_display();
        if (on_change)
            on_change(m_hsv);
    }

    HSV m_hsv;
    RefPtr<Bitmap> m_picture;
};

String hex_of(Color color)
{
    StringBuilder builder;
    builder.appendff("#{:02x}{:02x}{:02x}", color.red, color.green, color.blue);
    return MUST(builder.to_string());
}

bool color_from_hex(StringView text, Color& out)
{
    if (text.length() != 7 || text[0] != '#')
        return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    u8 values[3];
    for (size_t i = 0; i < 3; ++i) {
        int high = digit(text[1 + i * 2]);
        int low = digit(text[2 + i * 2]);
        if (high < 0 || low < 0)
            return false;
        values[i] = static_cast<u8>(high * 16 + low);
    }
    out = { values[0], values[1], values[2], 255 };
    return true;
}

}

ColorPanel& ColorPanel::the()
{
    static ColorPanel* panel = nullptr;
    if (!panel)
        panel = new ColorPanel;
    return *panel;
}

void ColorPanel::show(Color initial, Function<void(Color)> on_change)
{
    auto const& t = tokens();
    auto panel_width = static_cast<int>(t.metric("colorPanel.width"_sv));
    auto panel_height = static_cast<int>(t.metric("colorPanel.height"_sv));
    m_window = adopt_own(*new Window("Colors"_sv, { 120, 120, panel_width, panel_height }, WS::WindowTitled));
    auto& stack = m_window->set_content<StackView>(Axis::Vertical, 10, Alignment::Center, Insets::all(12));
    auto& wheel = stack.add<ColorWheel>();
    float wheel_size = t.metric("colorPanel.wheel"_sv);
    wheel.set_fixed_size({ wheel_size, wheel_size });
    auto& brightness = stack.add<Slider>(0, 1, 1);
    brightness.set_fixed_width(wheel_size);
    auto& hex = stack.add<TextField>(hex_of(initial).view(), FieldStyle::Rounded);
    hex.set_fixed_width(wheel_size);
    auto hsv = hsv_from_color(initial);
    wheel.set_hsv(hsv);
    brightness.set_value(hsv.value);

    struct Shared {
        HSV hsv;
        Function<void(Color)> on_change;
    };
    auto* shared = new Shared { hsv, move(on_change) };
    auto changed = [shared, &hex] {
        auto color = color_from_hsv(shared->hsv);
        hex.set_text(hex_of(color).view());
        if (shared->on_change)
            shared->on_change(color);
    };
    wheel.on_change = [shared, changed](HSV picked) {
        shared->hsv.hue = picked.hue;
        shared->hsv.saturation = picked.saturation;
        changed();
    };
    brightness.on_action = [shared, &wheel, &brightness, changed] {
        shared->hsv.value = brightness.value();
        wheel.set_hsv(shared->hsv);
        changed();
    };
    hex.on_commit = [shared, &wheel, &brightness, &hex] {
        Color color;
        if (!color_from_hex(hex.text(), color))
            return;
        shared->hsv = hsv_from_color(color);
        wheel.set_hsv(shared->hsv);
        brightness.set_value(shared->hsv.value);
        if (shared->on_change)
            shared->on_change(color);
    };
    m_window->on_closed = [this, shared] {
        delete shared;
        auto* raw = m_window.leak_ptr();
        Platform::the().defer([raw] { delete raw; });
    };
    m_window->show();
}

void ColorPanel::close()
{
    if (m_window)
        m_window->close();
}

// ---- Font panel

namespace {

struct FamilyList final : public TableModel {
    [[nodiscard]] size_t row_count() const override { return 2; }
    [[nodiscard]] String text_at(size_t row, size_t) const override { return MUST(String::from_view(row == 0 ? "Inter"_sv : "JetBrains Mono"_sv)); }
};

struct WeightList final : public TableModel {
    [[nodiscard]] size_t row_count() const override { return 4; }
    [[nodiscard]] String text_at(size_t row, size_t) const override
    {
        StringView names[4] = { "Regular"_sv, "Medium"_sv, "SemiBold"_sv, "Bold"_sv };
        return MUST(String::from_view(names[row]));
    }
};

class FontPreview final : public View {
public:
    explicit FontPreview(FontSpec spec)
        : m_spec(spec)
    {
    }

    void set_spec(FontSpec spec)
    {
        m_spec = spec;
        set_needs_display();
    }

    void draw(Graphics& graphics) override
    {
        graphics.fill({ 0, 0, width(), height() }, tokens().color("textBackground"_sv));
        graphics.draw_text_centered("The quick brown fox"_sv, m_spec, { 0, 0, width(), height() }, tokens().color("label"_sv));
    }

private:
    FontSpec m_spec;
};

}

FontPanel& FontPanel::the()
{
    static FontPanel* panel = nullptr;
    if (!panel)
        panel = new FontPanel;
    return *panel;
}

void FontPanel::show(FontSpec initial, Function<void(FontSpec)> on_change)
{
    auto const& t = tokens();
    auto panel_width = static_cast<int>(t.metric("fontPanel.width"_sv));
    auto panel_height = static_cast<int>(t.metric("fontPanel.height"_sv));
    m_window = adopt_own(*new Window("Fonts"_sv, { 160, 160, panel_width, panel_height }, WS::WindowTitled));
    auto& stack = m_window->set_content<StackView>(Axis::Vertical, 8, Alignment::Fill, Insets::all(12));
    auto& preview = stack.add<FontPreview>(initial);
    preview.set_fixed_height(56);
    auto& lists = stack.add<StackView>(Axis::Horizontal, 8);
    lists.set_stretch(0, 1);

    struct Shared {
        FontSpec spec;
        Function<void(FontSpec)> on_change;
        FamilyList families;
        WeightList weights;
    };
    auto* shared = new Shared { initial, move(on_change), {}, {} };
    auto& family_table = lists.add<TableView>(&shared->families);
    family_table.add_column("Family"_sv, 160);
    family_table.set_stretch(1, 0);
    auto& weight_table = lists.add<TableView>(&shared->weights);
    weight_table.add_column("Typeface"_sv, 120);
    weight_table.set_stretch(1, 0);
    auto& size_stack = stack.add<StackView>(Axis::Horizontal, 8, Alignment::Center);
    (void)size_stack.add<Label>("Size:"_sv);
    auto& size_slider = size_stack.add<Slider>(9, 72, initial.size);
    size_slider.set_stretch(1, 0);
    size_stack.set_fixed_height(20);
    family_table.select_row(initial.mono ? 1 : 0);
    weight_table.select_row(static_cast<size_t>(initial.weight));
    auto changed = [shared, &preview] {
        preview.set_spec(shared->spec);
        if (shared->on_change)
            shared->on_change(shared->spec);
    };
    family_table.on_selection_change = [shared, &family_table, changed] {
        shared->spec.mono = family_table.selected_row() == 1;
        changed();
    };
    weight_table.on_selection_change = [shared, &weight_table, changed] {
        shared->spec.weight = static_cast<FontWeight>(max(weight_table.selected_row(), 0));
        changed();
    };
    size_slider.on_action = [shared, &size_slider, changed] {
        shared->spec.size = static_cast<float>(static_cast<int>(size_slider.value() + 0.5f));
        changed();
    };
    m_window->on_closed = [this, shared] {
        delete shared;
        auto* raw = m_window.leak_ptr();
        Platform::the().defer([raw] { delete raw; });
    };
    m_window->show();
}

void FontPanel::close()
{
    if (m_window)
        m_window->close();
}

}
