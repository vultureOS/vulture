/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "HelpSearch.h"
#include "Notifications.h"
#include "Screenshots.h"
#include "SystemWindows.h"
#include "Widgets.h"
#include <WorkspaceEndpoint.h>
#include <Kernel/API/POSIX/privacy.h>
#include <PrivacyEndpoint.h>
#include <Desktop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibJSON/JSON.h>
#include <LibLocale/Locale.h>
#include <LibUI/Panels.h>
#include <LibUI/Application.h>
#include <LibUI/Controls.h>
#include <LibUI/Layout.h>
#include <LibUI/Menu.h>
#include <LibUI/Motion.h>
#include <LibUI/TextField.h>
#include <NotificationsEndpoint.h>
#include <SearchEndpoint.h>
#include <fcntl.h>
#include <notify.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Along the top of the screen: the system's mark, the active application's name and
// menus, and at the right SearchPanel, Quick Settings and the clock. Each of those opens
// a window of its own.

using namespace UI;
using namespace Gfx;

namespace {

bool s_still;

Appearance const& tokens()
{
    return Appearance::the();
}

// When --still, nothing about the clock may move: screenshots compare it.
String clock_text()
{
    if (s_still)
        return MUST(String::from_view("Thu Jan 15  9:41 AM"_sv));
    time_t const now = time(nullptr);
    struct tm parts;
    localtime_r(&now, &parts);
    // Written as where it's been set to write it. (It's asked every second, and that file doesn't change often: only look when it has.)
    static String s_preferences_path = [] {
        StringBuilder path;
        path.append(Desktop::home_directory().view());
        path.append("/Library/Preferences/com.vulture.desktop.json"_sv);
        return MUST(path.to_string());
    }();
    static time_t s_preferences_changed = -1;
    static Locale::LocaleData const* s_locale = &Locale::default_locale();
    struct stat information {};
    time_t const changed = stat(s_preferences_path.characters(), &information) == 0 ? information.st_mtime : 0;
    if (changed != s_preferences_changed) {
        s_preferences_changed = changed;
        Desktop::Preferences preferences("com.vulture.desktop"_sv);
        auto const* found = Locale::find_locale(preferences.string("region"_sv).value_or("en_US"_sv));
        s_locale = found ? found : &Locale::default_locale();
    }
    return MUST(Locale::format_date_pattern(*s_locale, StringView(s_locale->clock_format), now, static_cast<i32>(parts.tm_gmtoff)));
}

// ---- SearchPanel

struct SearchResult {
    String path;
    String name;
    String kind;
    String icon;
};

class ResultsView final : public View {
public:
    void set_results(Vector<SearchResult> results)
    {
        m_results = move(results);
        m_selected = m_results.is_empty() ? -1 : 0;
        set_needs_display();
    }

    [[nodiscard]] Vector<SearchResult> const& results() const { return m_results; }
    [[nodiscard]] int selected() const { return m_selected; }
    void move_selection(int by)
    {
        if (m_results.is_empty())
            return;
        m_selected = clamp(m_selected + by, 0, static_cast<int>(m_results.size()) - 1);
        set_needs_display();
    }

    Function<void(size_t)> on_activate;

    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        float const row = tokens().metric("searchPanel.rowHeight"_sv);
        for (size_t i = 0; i < m_results.size(); ++i)
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Row, m_results[i].name.view(), { 0, static_cast<float>(i) * row, width(), row }, static_cast<int>(i) == m_selected ? "selected"_sv : StringView {}));
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float row = t.metric("searchPanel.rowHeight"_sv);
        float icon_size = t.metric("searchPanel.iconSize"_sv);
        float padding = t.metric("searchPanel.padding"_sv);
        auto font = t.named_font("body"_sv);
        float cap = Appearance::cap_height(font);
        for (size_t i = 0; i < m_results.size(); ++i) {
            auto const& result = m_results[i];
            float y = static_cast<float>(i) * row;
            bool selected = static_cast<int>(i) == m_selected;
            if (selected)
                graphics.fill_rounded({ 6, y + 1, width() - 12, row - 2 }, 8, t.color("searchPanel.selection"_sv));
            auto icon = Desktop::icon_named(result.icon.view(), static_cast<int>(icon_size), static_cast<int>(graphics.scale()));
            if (icon)
                graphics.painter().draw_bitmap(*icon, FloatRect { padding, y + (row - icon_size) / 2, icon_size, icon_size });
            auto color = t.color(selected ? "searchPanel.selectionText"_sv : "label"_sv);
            float text_x = padding + icon_size + 10;
            float baseline = y + (row + cap) / 2;
            graphics.draw_text(result.name.view(), font, { text_x, baseline }, color);
            auto kind_width = Appearance::text_width(font, result.kind.view());
            graphics.draw_text(result.kind.view(), font, { width() - padding - kind_width, baseline }, selected ? color : t.color("secondaryLabel"_sv));
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        auto row = static_cast<int>(event.position.y / tokens().metric("searchPanel.rowHeight"_sv));
        if (row < 0 || static_cast<size_t>(row) >= m_results.size())
            return false;
        m_selected = row;
        set_needs_display();
        if (on_activate)
            on_activate(static_cast<size_t>(row));
        return true;
    }

private:
    Vector<SearchResult> m_results;
    int m_selected { -1 };
};

class SearchPanelField final : public TextField {
public:
    SearchPanelField()
        : TextField({}, FieldStyle::Plain)
    {
    }

    Function<bool(KeyEvent const&)> on_key;

    bool key_down(KeyEvent const& event) override
    {
        if (on_key && on_key(event))
            return true;
        return TextField::key_down(event);
    }

    [[nodiscard]] bool shows_focus_ring() const override { return false; }
    void draw(Graphics& graphics) override
    {
        // As a field, at the size of SearchPanel: the toolkit's own, drawn large.
        auto const& t = tokens();
        FontSpec font { t.metric("searchPanel.fontSize"_sv), FontWeight::Regular, false };
        float baseline = (height() + Appearance::cap_height(font)) / 2;
        if (text().is_empty()) {
            graphics.draw_text("Search"_sv, font, { 0, baseline }, t.color("placeholder"_sv));
        } else {
            graphics.draw_text(text(), font, { 0, baseline }, t.color("label"_sv));
        }
        if (is_first_responder()) {
            float x = Appearance::text_width(font, text().substring_view(0, storage().caret()));
            graphics.fill({ graphics.snap(x), baseline - Appearance::ascent(font), 2, Appearance::line_height(font) }, t.color("accent"_sv));
        }
    }
};

class SearchPanel {
public:
    SearchPanel()
    {
        auto const& t = tokens();
        auto screen = Platform::the().screen_size();
        auto width = static_cast<int>(t.metric("searchPanel.width"_sv));
        auto field_height = static_cast<int>(t.metric("searchPanel.fieldHeight"_sv));
        auto row = static_cast<int>(t.metric("searchPanel.rowHeight"_sv));
        auto rows = static_cast<int>(t.metric("searchPanel.maxResults"_sv));
        m_full_height = field_height + row * rows + 8;
        m_window = adopt_own(*new UI::Window("Search"_sv,
            { (screen.width - width) / 2, static_cast<int>(static_cast<float>(screen.height) * t.metric("searchPanel.topFraction"_sv)), width, m_full_height },
            WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
        m_window->set_transparent(true);
        m_window->backend().set_level(WS::LevelPopup);
        auto& face = m_window->root_view().add<Face>(*this);
        face.set_frame({ 0, 0, static_cast<float>(width), static_cast<float>(m_full_height) });
        m_face = &face;
        float padding = t.metric("searchPanel.padding"_sv);
        m_field = &face.add<SearchPanelField>();
        m_field->set_frame({ padding + 34, 0, static_cast<float>(width) - padding * 2 - 34, static_cast<float>(field_height) });
        m_results = &face.add<ResultsView>();
        m_results->set_frame({ 0, static_cast<float>(field_height) + 4, static_cast<float>(width), static_cast<float>(row * rows) });
        m_results->on_activate = [this](size_t index) { open(index); };
        m_field->on_change = [this] { search(); };
        m_field->on_key = [this](KeyEvent const& event) {
            switch (event.code) {
            case KeyDown:
                m_results->move_selection(1);
                return true;
            case KeyUp:
                m_results->move_selection(-1);
                return true;
            case KeyEnter:
                if (m_results->selected() >= 0)
                    open(static_cast<size_t>(m_results->selected()));
                return true;
            case KeyEscape:
                hide();
                return true;
            default:
                return false;
            }
        };
        m_window->on_key_change = [this](bool active) {
            if (!active && m_shown)
                hide();
        };
        resize_for_results();
    }

    void toggle()
    {
        if (m_shown)
            hide();
        else
            show();
    }

    void show()
    {
        m_shown = true;
        m_field->set_text({});
        m_results->set_results({});
        resize_for_results();
        m_window->show();
        m_window->make_first_responder(m_field);
        // So that what was only just saved is there to be found.
        if (auto port = IPC::look_up("com.vulture.indexd"_sv); !port.is_error()) {
            SearchProxy search_service(port.release_value());
            (void)search_service.update();
        }
        printf("search: shown\n");
        fflush(stdout);
    }

    void hide()
    {
        m_shown = false;
        m_window->hide();
    }

private:
    class Face final : public View {
    public:
        explicit Face(SearchPanel& search_panel)
            : m_search_panel(search_panel)
        {
        }

        void draw(Graphics& graphics) override
        {
            auto const& t = tokens();
            float shown = static_cast<float>(m_search_panel.shown_height());
            float radius = t.metric("searchPanel.radius"_sv);
            FloatRect shape { 0, 0, width(), shown };
            graphics.fill_rounded(shape, radius, t.color("searchPanel.fill"_sv));
            graphics.stroke_rounded(shape, radius, graphics.pixel(), t.color("searchPanel.border"_sv));
            float field = t.metric("searchPanel.fieldHeight"_sv);
            float padding = t.metric("searchPanel.padding"_sv);
            graphics.draw_symbol(Symbol::Magnifier, { padding, (field - 24) / 2, 24, 24 }, t.color("secondaryLabel"_sv), 1.4f);
            if (shown > field)
                graphics.fill({ 0, field, width(), 1 }, t.color("separator"_sv));
        }

    private:
        SearchPanel& m_search_panel;
    };

    [[nodiscard]] int shown_height() const
    {
        auto const& t = tokens();
        auto field = static_cast<int>(t.metric("searchPanel.fieldHeight"_sv));
        auto count = static_cast<int>(m_results->results().size());
        return count == 0 ? field : field + 8 + count * static_cast<int>(t.metric("searchPanel.rowHeight"_sv));
    }

    void resize_for_results()
    {
        auto const& t = tokens();
        auto height = shown_height();
        MaterialRegion material { { 0, 0, m_window->frame().width, height }, WS::Material::Popover, static_cast<int>(t.metric("searchPanel.radius"_sv)) };
        m_window->set_materials(Span<MaterialRegion const>(&material, 1));
        m_window->backend().set_input_region({ 0, 0, m_window->frame().width, height });
        m_face->set_needs_display();
    }

    void search()
    {
        Vector<SearchResult> results;
        auto text = m_field->text();
        size_t limit = static_cast<size_t>(tokens().metric("searchPanel.maxResults"_sv));
        if (!text.is_empty()) {
            auto port = IPC::look_up("com.vulture.indexd"_sv);
            if (!port.is_error()) {
                SearchProxy search_service(port.release_value());
                auto answer = search_service.query(text, static_cast<u32>(limit));
                if (!answer.is_error()) {
                    auto json = JSON::parse(answer.value().view());
                    if (!json.is_error()) {
                        for (size_t i = 0; i < json.value().size(); ++i) {
                            auto const& one = json.value().at(i);
                            SearchResult result;
                            auto path = one.get_string("path"_sv).value_or(StringView {});
                            result.path = MUST(String::from_view(path));
                            auto name = one.get_string("name"_sv).value_or(StringView {});
                            if (name.ends_with(".app"_sv))
                                name = name.substring_view(0, name.length() - 4);
                            result.name = MUST(String::from_view(name));
                            auto kind = one.get_string("kind"_sv).value_or(StringView {});
                            result.kind = MUST(String::from_view(kind == "application"_sv ? "Application"_sv : (kind == "folder"_sv ? "Folder"_sv : "Document"_sv)));
                            result.icon = Desktop::icon_name_for_path(path);
                            results.append(move(result));
                        }
                    }
                }
            }
        }
        m_results->set_results(move(results));
        resize_for_results();
    }

    void open(size_t index)
    {
        auto const& result = m_results->results()[index];
        (void)Desktop::open_path(result.path.view());
        hide();
    }

    OwnPtr<UI::Window> m_window;
    View* m_face { nullptr };
    SearchPanelField* m_field { nullptr };
    ResultsView* m_results { nullptr };
    bool m_shown { false };
    int m_full_height { 0 };
};

// ---- Privacy: someone's question, on its own above everything.

class ConsentPanel final : public PrivacyPromptsStub {
public:
    ConsentPanel()
    {
        auto port = IPC::look_up("com.vulture.privacyd"_sv);
        if (port.is_error())
            return;
        m_service = adopt_own(*new PrivacyProxy(port.release_value()));
        auto pair = IPC::create_port();
        if (pair.is_error())
            return;
        auto ports = pair.release_value();
        m_listener = adopt_own(*new IPC::Listener(Application::the().event_loop(), move(ports.receive), [this](IPC::ReceivedMessage& message) { (void)handle(message); }));
        (void)m_service->subscribe(move(ports.send));
    }

    void ask(IPC::Sender const&, u32 id, String application, u32 service) override
    {
        m_questions.append(Question { id, move(application), service });
        if (m_questions.size() == 1)
            show_next();
    }

private:
    struct Question {
        u32 id;
        String application;
        u32 service;
    };

    class Icon final : public View {
    public:
        explicit Icon(RefPtr<Bitmap> bitmap)
            : m_bitmap(move(bitmap))
        {
        }

        [[nodiscard]] FloatSize intrinsic_size() const override
        {
            float const size = tokens().metric("alert.iconSize"_sv);
            return { size, size };
        }

        void draw(Graphics& graphics) override
        {
            if (m_bitmap)
                graphics.painter().draw_bitmap(*m_bitmap, FloatRect { 0, 0, width(), height() });
            else
                graphics.draw_symbol(Symbol::Question, { 0, 0, width(), height() }, tokens().color("accent"_sv), 3);
        }

    private:
        RefPtr<Bitmap> m_bitmap;
    };

    // A window with nothing around it has no shadow from the window server, so the shadow is its own.
    static constexpr float shadow_margin = 24;

    class Face final : public View {
    public:
        void draw(Graphics& graphics) override
        {
            UI::draw_sheet_face(graphics, { shadow_margin, shadow_margin, width() - shadow_margin * 2, height() - shadow_margin * 2 });
        }
    };

    void show_next()
    {
        if (m_questions.is_empty())
            return;
        auto const& question = m_questions.first();
        auto const& t = tokens();

        StringView name = question.application.view();
        if (auto const slash = name.find_last('/'); slash.has_value())
            name = name.substring_view(slash.value() + 1);
        if (name.ends_with(".app"_sv))
            name = name.substring_view(0, name.length() - 4);
        RefPtr<Bitmap> icon;
        if (auto bundle = Desktop::read_bundle(question.application.view()); !bundle.is_error()) {
            icon = Desktop::icon_named(bundle.value().icon.view(), static_cast<int>(t.metric("alert.iconSize"_sv)), Platform::the().scale());
            if (!bundle.value().name.is_empty())
                name = {};
            StringBuilder title;
            title.append(name.is_empty() ? bundle.value().name.view() : name);
            m_name = MUST(title.to_string());
        } else {
            m_name = MUST(String::from_view(name));
        }

        StringView folder;
        switch (question.service) {
        case VULTURE_PRIVACY_DESKTOP:
            folder = "your Desktop folder"_sv;
            break;
        case VULTURE_PRIVACY_DOCUMENTS:
            folder = "your Documents folder"_sv;
            break;
        case VULTURE_PRIVACY_SCREEN:
            folder = "what's on your screen"_sv;
            break;
        case VULTURE_PRIVACY_DOWNLOADS:
            folder = "your Downloads folder"_sv;
            break;
        default:
            folder = "something of yours"_sv;
            break;
        }
        StringBuilder message;
        message.appendff("\u201C{}\u201D would like to access files in {}.", m_name.view(), folder);

        float const padding = t.metric("alert.padding"_sv);
        float const panel_width = t.metric("alert.width"_sv);
        float const inner_width = panel_width - padding * 2;
        float const spacing = t.metric("alert.spacing"_sv);
        auto const headline = t.named_font("headline"_sv);
        auto const small = t.named_font("small"_sv);

        // Sized by what it holds, once that has been laid out to the width.
        auto measure = [&](StringView text, FontSpec const& font) {
            auto* probe = new Label(text, font);
            probe->set_wraps(true);
            probe->set_frame({ 0, 0, inner_width, Appearance::line_height(font) });
            float const height = probe->intrinsic_size().height;
            delete probe;
            return height;
        };
        constexpr StringView explanation = "You can change this later in Settings, under Privacy & Security."_sv;
        float const message_height = measure(message.view(), headline);
        float const explanation_height = measure(explanation, small);
        float const button_height = t.metric("alert.buttonHeight"_sv);
        float const height = padding * 2 + t.metric("alert.iconSize"_sv) + message_height + explanation_height + button_height + spacing * 3;

        auto screen = Platform::the().screen_size();
        int const margin = static_cast<int>(shadow_margin);
        IntRect const frame { (screen.width - static_cast<int>(panel_width)) / 2 - margin, (screen.height - static_cast<int>(height)) / 3 - margin, static_cast<int>(panel_width) + margin * 2, static_cast<int>(height) + margin * 2 };
        m_window = adopt_own(*new UI::Window("Privacy"_sv, frame, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
        m_window->set_transparent(true);
        m_window->backend().set_level(WS::LevelPopup);
        m_window->backend().set_input_region({ margin, margin, static_cast<int>(panel_width), static_cast<int>(height) });
        auto& face = m_window->root_view().add<Face>();
        face.set_frame({ 0, 0, static_cast<float>(frame.width), static_cast<float>(frame.height) });
        auto& stack = face.add<StackView>(Axis::Vertical, spacing, Alignment::Center, Insets::all(padding));
        stack.set_frame({ shadow_margin, shadow_margin, panel_width, height });
        (void)stack.add<Icon>(icon);
        auto& message_label = stack.add<Label>(message.view(), headline);
        message_label.set_alignment(TextAlignment::Center);
        message_label.set_wraps(true);
        message_label.set_fixed_size({ inner_width, message_height });
        auto& explanation_label = stack.add<Label>(explanation, small);
        explanation_label.set_alignment(TextAlignment::Center);
        explanation_label.set_wraps(true);
        explanation_label.set_fixed_size({ inner_width, explanation_height });
        auto& row = stack.add<StackView>(Axis::Horizontal, t.metric("alert.buttonSpacing"_sv));
        row.stack().set_distribution(Distribution::Equal);
        row.set_fixed_size({ inner_width, button_height });
        auto& refuse = row.add<Button>("Don\u2019t Allow"_sv);
        refuse.set_control_size(ControlSize::Large);
        refuse.on_action = [this] { answer(false); };
        auto& allow = row.add<Button>("Allow"_sv, ButtonStyle::Default);
        allow.set_control_size(ControlSize::Large);
        allow.on_action = [this] { answer(true); };
        Platform::the().play_sound("alert"_sv);
        m_window->show();
        m_window->backend().activate();
        printf("privacy: asking about %s\n", m_name.characters());
        fflush(stdout);
    }

    void answer(bool allowed)
    {
        if (m_questions.is_empty())
            return;
        (void)m_service->answer(m_questions.first().id, allowed);
        m_questions.remove(0);
        // Not from inside its own button's press.
        auto* closing = m_window.leak_ptr();
        if (closing) {
            closing->hide();
            Platform::the().defer([closing] { delete closing; });
        }
        printf("privacy: answered\n");
        fflush(stdout);
        show_next();
    }

    OwnPtr<PrivacyProxy> m_service;
    OwnPtr<IPC::Listener> m_listener;
    OwnPtr<UI::Window> m_window;
    Vector<Question> m_questions;
    String m_name;
};

// ---- Starting applications for what can't have them as its own children.

class WorkspaceService final : public WorkspaceStub {
public:
    WorkspaceService()
    {
        auto pair = IPC::create_port();
        if (pair.is_error())
            return;
        auto ports = pair.release_value();
        m_listener = adopt_own(*new IPC::Listener(Application::the().event_loop(), move(ports.receive), [this](IPC::ReceivedMessage& message) { (void)handle(message); }));
        (void)IPC::register_service("com.vulture.workspace"_sv, move(ports.send));
    }

    WorkspaceEndpoint::OpenApplicationAnswer open_application(IPC::Sender const& sender, String bundle) override
    {
        // Someone else's session isn't this one's business.
        if (sender.uid != getuid())
            return { EPERM, -1 };
        if (!bundle.view().ends_with(".app"_sv))
            return { EINVAL, -1 };
        auto info = Desktop::read_bundle(bundle.view());
        if (info.is_error())
            return { info.error().code(), -1 };
        auto launched = Desktop::launch(info.value());
        if (launched.is_error())
            return { launched.error().code(), -1 };
        return { 0, launched.value() };
    }

private:
    OwnPtr<IPC::Listener> m_listener;
};

// ---- Quick Settings

class Tile final : public Control {
public:
    Tile(StringView title, Symbol symbol, bool on)
        : m_title(MUST(String::from_view(title)))
        , m_symbol(symbol)
        , m_on(on)
    {
    }

    void set_on(bool on)
    {
        m_on = on;
        set_needs_display();
    }
    [[nodiscard]] bool is_on() const { return m_on; }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        graphics.fill_rounded({ 0, 0, width(), height() }, t.metric("quickSettings.tileRadius"_sv), t.color("quickSettings.tile"_sv));
        float circle = 30;
        FloatPoint center { 12 + circle / 2, height() / 2 };
        graphics.fill_circle(center, circle / 2, m_on ? t.color("quickSettings.tileOn"_sv) : t.color("quickSettings.tile"_sv));
        auto symbol_color = m_on ? Color { 255, 255, 255, 255 } : t.color("label"_sv);
        graphics.draw_symbol(m_symbol, { center.x - 9, center.y - 9, 18, 18 }, symbol_color, 1.8f);
        auto font = t.named_font("callout"_sv);
        font.weight = FontWeight::Medium;
        graphics.draw_text_truncated(m_title.view(), font, { 12 + circle + 8, height() / 2 + 4 }, width() - 12 - circle - 12, t.color("label"_sv));
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Switch; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_title.view())); }
    [[nodiscard]] String accessibility_value() const override { return MUST(String::from_view(m_on ? "on"_sv : "off"_sv)); }

private:
    String m_title;
    Symbol m_symbol;
    bool m_on;
};

// A slider with a name over it, in a tile as wide as the panel.
class SliderTile final : public View {
public:
    explicit SliderTile(StringView title)
        : m_title(MUST(String::from_view(title)))
    {
        m_slider = &add<Slider>(0.0f, 100.0f, 0.0f);
    }

    [[nodiscard]] Slider& slider() { return *m_slider; }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        graphics.fill_rounded({ 0, 0, width(), height() }, t.metric("quickSettings.tileRadius"_sv), t.color("quickSettings.tile"_sv));
        auto font = t.named_font("callout"_sv);
        font.weight = FontWeight::Medium;
        graphics.draw_text_truncated(m_title.view(), font, { 12, 21 }, width() - 24, t.color("label"_sv));
    }

    void layout() override
    {
        float slider_height = m_slider->intrinsic_size().height;
        m_slider->set_frame({ 12, height() - slider_height - 10, width() - 24, slider_height });
    }

private:
    String m_title;
    Slider* m_slider { nullptr };
};

// Cmd-Tab: every application, the most recent first, with one chosen. It goes when Cmd does, and it's the server that decides.
class SwitcherView final : public View {
public:
    struct Entry {
        i32 pid { 0 };
        String name;
        String icon;
    };

    void set(Vector<Entry> entries, int selected)
    {
        m_entries = move(entries);
        m_selected = selected;
        set_needs_display();
    }

    [[nodiscard]] FloatSize size_wanted() const
    {
        auto const& t = tokens();
        return { t.metric("switcher.padding"_sv) * 2 + t.metric("switcher.cell"_sv) * static_cast<float>(m_entries.size()), t.metric("switcher.padding"_sv) * 2 + t.metric("switcher.cell"_sv) + t.metric("switcher.label"_sv) };
    }

private:
    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float const padding = t.metric("switcher.padding"_sv);
        float const cell = t.metric("switcher.cell"_sv);
        float const icon = t.metric("switcher.icon"_sv);
        for (size_t i = 0; i < m_entries.size(); ++i) {
            FloatRect const place { padding + cell * static_cast<float>(i), padding, cell, cell };
            if (static_cast<int>(i) == m_selected)
                graphics.fill_rounded(place, t.metric("switcher.radius"_sv), t.color("switcher.selected"_sv));
            // (At the size it's shown: a smaller one made bigger is blurred.)
            if (auto picture = Desktop::icon_named(m_entries[i].icon.view(), static_cast<int>(icon), static_cast<int>(graphics.scale())))
                graphics.painter().draw_bitmap(*picture, { place.x + (cell - icon) / 2, place.y + (cell - icon) / 2, icon, icon });
        }
        if (m_selected >= 0 && m_selected < static_cast<int>(m_entries.size())) {
            FloatRect const label { 0, padding + cell, width(), t.metric("switcher.label"_sv) };
            graphics.draw_text_centered(m_entries[static_cast<size_t>(m_selected)].name.view(), t.named_font("body"_sv), label, t.color("label"_sv));
        }
    }

    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        auto const& t = tokens();
        float const padding = t.metric("switcher.padding"_sv);
        float const cell = t.metric("switcher.cell"_sv);
        for (size_t i = 0; i < m_entries.size(); ++i) {
            StringView const value = static_cast<int>(i) == m_selected ? "chosen"_sv : StringView {};
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Cell, m_entries[i].name.view(), { padding + cell * static_cast<float>(i), padding, cell, cell }, value));
        }
    }

    Vector<Entry> m_entries;
    int m_selected { 0 };
};

class Switcher {
public:
    Switcher()
    {
        auto screen = Platform::the().screen_size();
        m_window = adopt_own(*new UI::Window("Switcher"_sv, { 0, 0, 200, 200 }, WS::WindowBare | WS::WindowPopup));
        m_window->set_transparent(true);
        m_window->backend().set_level(WS::LevelPopup);
        m_view = &m_window->root_view().add<SwitcherView>();
        (void)screen;
    }

    void update(Desktop::ShellState const& state)
    {
        if (state.switcher.is_empty()) {
            m_window->hide();
            // Ready for when it's wanted: what each one running looks like, found now rather than then.
            int const scale = m_window->backend().scale();
            int const size = static_cast<int>(tokens().metric("switcher.icon"_sv));
            for (auto const& application : state.applications) {
                if (!application.identifier.is_empty())
                    (void)Desktop::icon_named(icon_of(application.identifier.view()).view(), size, scale);
            }
            return;
        }
        Vector<SwitcherView::Entry> entries;
        for (auto pid : state.switcher) {
            for (auto const& application : state.applications) {
                if (application.pid != pid)
                    continue;
                SwitcherView::Entry entry;
                entry.pid = pid;
                entry.name = MUST(String::from_view(application.name.view()));
                entry.icon = icon_of(application.identifier.view());
                entries.append(move(entry));
            }
        }
        m_view->set(move(entries), state.switcher_selected);
        auto const wanted = m_view->size_wanted();
        auto const screen = Platform::the().screen_size();
        Gfx::IntRect const frame { (screen.width - static_cast<int>(wanted.width)) / 2, (screen.height - static_cast<int>(wanted.height)) / 2, static_cast<int>(wanted.width), static_cast<int>(wanted.height) };
        m_window->set_frame(frame);
        float const radius = tokens().metric("switcher.panelRadius"_sv);
        MaterialRegion material { { 0, 0, frame.width, frame.height }, WS::Material::HUD, static_cast<int>(radius) };
        m_window->set_materials(Span<MaterialRegion const>(&material, 1));
        m_view->set_frame({ 0, 0, wanted.width, wanted.height });
        m_window->show();
    }

private:
    // Which icon an application has, kept: finding out reads every application's bundle, and the panel has to be
    // there at once.
    String icon_of(StringView identifier)
    {
        for (auto const& known : m_icons) {
            if (known.identifier.view() == identifier)
                return MUST(String::from_view(known.icon.view()));
        }
        auto bundle = Desktop::application_with_identifier(identifier);
        auto icon = MUST(String::from_view(bundle.has_value() ? bundle.value().icon.view() : "GenericApplication"_sv));
        m_icons.append({ MUST(String::from_view(identifier)), MUST(String::from_view(icon.view())) });
        return icon;
    }

    struct KnownIcon {
        String identifier;
        String icon;
    };
    Vector<KnownIcon> m_icons;
    OwnPtr<UI::Window> m_window;
    SwitcherView* m_view { nullptr };
};

class QuickSettingsPanel {
public:
    QuickSettingsPanel()
    {
        auto const& t = tokens();
        auto screen = Platform::the().screen_size();
        auto width = static_cast<int>(t.metric("quickSettings.width"_sv));
        auto height = static_cast<int>(t.metric("quickSettings.height"_sv));
        auto top = static_cast<int>(t.metric("menubar.height"_sv)) + 4;
        m_window = adopt_own(*new UI::Window("Quick Settings"_sv, { screen.width - width - 8, top, width, height }, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
        m_window->set_transparent(true);
        m_window->backend().set_level(WS::LevelPopup);
        MaterialRegion material { { 0, 0, width, height }, WS::Material::Popover, static_cast<int>(t.metric("quickSettings.radius"_sv)) };
        m_window->set_materials(Span<MaterialRegion const>(&material, 1));
        auto& face = m_window->root_view().add<Face>();
        face.set_frame({ 0, 0, static_cast<float>(width), static_cast<float>(height) });
        float padding = t.metric("quickSettings.padding"_sv);
        auto& stack = face.add<StackView>(Axis::Vertical, 8, Alignment::Fill, Insets::all(padding));
        stack.set_frame({ 0, 0, static_cast<float>(width), static_cast<float>(height) });
        float tile_height = t.metric("quickSettings.tileHeight"_sv);
        auto& dark = stack.add<Tile>("Dark Mode"_sv, Symbol::ClearCircle, Appearance::the().is_dark());
        dark.set_fixed_height(tile_height);
        dark.on_action = [&dark] {
            bool to_dark = !Appearance::the().is_dark();
            (void)Application::the().connection().server().set_appearance(to_dark ? "Dark"_sv : "Light"_sv);
            dark.set_on(to_dark);
            Desktop::Preferences preferences("com.vulture.desktop"_sv);
            preferences.set("appearance"_sv, MUST(JSON::Value::string(to_dark ? "Dark"_sv : "Light"_sv)));
            preferences.save();
        };
        // Focus, as Sequoia's module is: Do Not Disturb, kept across logins, and said in the menu bar while it's on.
        Desktop::Preferences focus("com.vulture.focus"_sv);
        auto& quiet = stack.add<Tile>("Focus"_sv, Symbol::Moon, !focus.string("mode"_sv).value_or(StringView {}).is_empty());
        quiet.set_fixed_height(tile_height);
        m_quiet = &quiet;
        quiet.on_action = [this, &quiet] { set_focus(!quiet.is_on()); };
        auto& sound = stack.add<SliderTile>("Sound"_sv);
        sound.set_fixed_height(t.metric("quickSettings.sliderTileHeight"_sv));
        m_volume = &sound.slider();
        m_volume->on_action = [this] { Application::the().set_output_volume(static_cast<u32>(m_volume->value() + 0.5f)); };
        auto& settings = stack.add<Tile>("Settings…"_sv, Symbol::Sidebar, false);
        settings.set_fixed_height(tile_height);
        settings.on_action = [this] {
            (void)Desktop::open_path("/System/Applications/Settings.app"_sv);
            m_shown = false;
            m_window->hide();
        };
        m_window->on_key_change = [this](bool active) {
            if (!active && m_shown) {
                m_shown = false;
                m_window->hide();
            }
        };
    }

    void toggle()
    {
        m_shown = !m_shown;
        if (m_shown) {
            // Something else may have changed it.
            auto percent = Application::the().output_volume();
            m_volume->set_value(static_cast<float>(percent.value_or(0)));
            m_volume->set_enabled(percent.has_value());
            m_window->show();
        } else {
            m_window->hide();
        }
    }

    [[nodiscard]] bool is_quiet() const { return m_quiet->is_on(); }

    // From here, or from Settings: Do Not Disturb, or nothing.
    void set_focus(bool on)
    {
        m_quiet->set_on(on);
        Desktop::Preferences focus("com.vulture.focus"_sv);
        focus.set("mode"_sv, MUST(JSON::Value::string(on ? "Do Not Disturb"_sv : ""_sv)));
        focus.save();
        if (on_focus_change)
            on_focus_change();
    }
    void reload_focus()
    {
        Desktop::Preferences focus("com.vulture.focus"_sv);
        m_quiet->set_on(!focus.string("mode"_sv).value_or(StringView {}).is_empty());
        if (on_focus_change)
            on_focus_change();
    }
    Function<void()> on_focus_change;

private:
    class Face final : public View {
    public:
        void draw(Graphics& graphics) override
        {
            auto const& t = tokens();
            graphics.fill_rounded({ 0, 0, width(), height() }, t.metric("quickSettings.radius"_sv), t.color("quickSettings.fill"_sv));
        }
    };

    OwnPtr<UI::Window> m_window;
    Tile* m_quiet { nullptr };
    Slider* m_volume { nullptr };
    bool m_shown { false };
};

// ---- The bar

class MenuBarView final : public View {
public:
    // Banners give way to what opens from here.
    Function<void()> on_panel_opening;
    // The system menu's own windows.
    Function<void()> on_about;
    Function<void()> on_force_quit;

    MenuBarView(SearchPanel& search_panel, DesktopShell::Notifications& center, QuickSettingsPanel& control)
        : m_search_panel(search_panel)
        , m_center(center)
        , m_control(control)
    {
        // Sequoia's, in its order. (Recent Items, Sleep and Lock Screen come with M26, M45 and M25.)
        m_system_menu = adopt_own(*new Menu);
        m_system_menu->add("About This Computer"_sv, [this] {
            if (on_about)
                on_about();
        });
        m_system_menu->add_separator();
        m_system_menu->add("Settings…"_sv, [] { (void)Desktop::open_path("/System/Applications/Settings.app"_sv); });
        m_system_menu->add("Software…"_sv, [] { (void)Desktop::open_path("/System/Applications/Software.app"_sv); });
        m_system_menu->add_separator();
        m_system_menu->add_submenu("Recent Items"_sv);
        m_system_menu->items()[m_system_menu->items().size() - 1].enabled = false;
        m_system_menu->add_separator();
        m_system_menu->add("Force Quit…"_sv, [this] {
            if (on_force_quit)
                on_force_quit();
        }).shortcut_text = MUST(String::from_view("\xE2\x8C\xA5\xE2\x8C\x98\xE2\x8E\x8B"_sv));
        m_system_menu->add_separator();
        m_system_menu->add("Sleep"_sv).enabled = false;
        m_system_menu->add("Restart…"_sv, [] { Desktop::restart(); });
        m_system_menu->add("Shut Down…"_sv, [] { Desktop::shut_down(); });
        m_system_menu->add_separator();
        auto& lock = m_system_menu->add("Lock Screen"_sv);
        lock.enabled = false;
        lock.shortcut_text = MUST(String::from_view("\xE2\x8C\x83\xE2\x8C\x98Q"_sv));
        struct passwd const* account = getpwuid(getuid());
        StringBuilder log_out;
        log_out.appendff("Log Out {}…", account ? StringView { account->pw_name, strlen(account->pw_name) } : "vulture"_sv);
        m_system_menu->add(log_out.view(), [] { Desktop::log_out(); }).shortcut_text = MUST(String::from_view("\xE2\x87\xA7\xE2\x8C\x98Q"_sv));
    }

    void set_state(Desktop::ShellState state)
    {
        // Windows come and go, this menu's own among them: only a change in the menus
        // themselves touches what is open.
        bool menus_changed = state.active_pid != m_state.active_pid || state.menus.view() != m_state.menus.view() || state.active_name.view() != m_state.active_name.view();
        m_state = move(state);
        if (menus_changed || m_items.is_empty())
            rebuild_menus();
        set_needs_display();
    }

    void accessibility_parts(Vector<Accessibility::Node>& parts) override
    {
        for (auto const& item : m_items)
            parts.append(Accessibility::part_of(*this, Accessibility::Role::MenuItem, item.is_logo ? "Logo"_sv : item.title.view(), { item.left, 0, item.width, height() }));
        parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, "Search"_sv, m_search_rect));
        if (!m_focus_rect.is_empty())
            parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, "Focus"_sv, m_focus_rect, "Do Not Disturb"_sv));
        parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, "Quick Settings"_sv, m_control_rect));
        parts.append(Accessibility::part_of(*this, Accessibility::Role::Button, "Notifications"_sv, m_clock_rect, m_clock.view()));
    }

    void tick_clock()
    {
        auto now = clock_text();
        if (now.view() != m_clock.view()) {
            m_clock = move(now);
            set_needs_display();
        }
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        auto text = t.color("menubar.text"_sv);
        auto font = t.named_font("menu"_sv);
        auto bold = t.named_font("headline"_sv);
        float cap = Appearance::cap_height(font);
        float baseline = (height() + cap) / 2;
        float inset = t.metric("menubar.highlightInset"_sv);
        float radius = t.metric("menubar.highlightRadius"_sv);
        layout_items();
        for (size_t i = 0; i < m_items.size(); ++i) {
            auto const& item = m_items[i];
            if (static_cast<int>(i) == m_open)
                graphics.fill_rounded({ item.left, inset / 2, item.width, height() - inset }, radius, t.color("menubar.highlight"_sv));
            if (item.is_logo) {
                float logo = t.metric("menubar.logoSize"_sv);
                Desktop::draw_logo(graphics, { item.left + (item.width - logo) / 2, (height() - logo) / 2, logo, logo }, text);
                continue;
            }
            float padding = t.metric("menubar.itemPadding"_sv);
            graphics.draw_text(item.title.view(), item.bold ? bold : font, { item.left + padding, baseline }, text);
        }
        // From the right: the clock, Quick Settings, SearchPanel.
        float right = width() - t.metric("menubar.rightPadding"_sv);
        float clock_width = Appearance::text_width(font, m_clock.view());
        m_clock_rect = { right - clock_width - 6, 0, clock_width + 12, height() };
        graphics.draw_text(m_clock.view(), font, { right - clock_width, baseline }, text);
        right -= clock_width + t.metric("menubar.statusGap"_sv);
        float symbol = 16;
        m_control_rect = { right - symbol - 4, 0, symbol + 8, height() };
        graphics.line({ right - symbol + 1, height() / 2 - 3.5f }, { right - 1, height() / 2 - 3.5f }, 2.6f, text, true);
        graphics.fill_circle({ right - 4, height() / 2 - 3.5f }, 2.6f, text);
        graphics.line({ right - symbol + 1, height() / 2 + 3.5f }, { right - 1, height() / 2 + 3.5f }, 2.6f, text, true);
        graphics.fill_circle({ right - symbol + 4, height() / 2 + 3.5f }, 2.6f, text);
        right -= symbol + t.metric("menubar.statusGap"_sv);
        m_search_rect = { right - symbol - 4, 0, symbol + 8, height() };
        graphics.draw_symbol(Symbol::Magnifier, { right - symbol, (height() - symbol) / 2, symbol, symbol }, text, 1.5f);
        // A Focus that's on, to the left of Search.
        m_focus_rect = {};
        if (m_control.is_quiet() && m_shows_focus) {
            right -= symbol + t.metric("menubar.statusGap"_sv);
            m_focus_rect = { right - symbol - 4, 0, symbol + 8, height() };
            graphics.draw_symbol(Symbol::Moon, { right - symbol, (height() - symbol) / 2, symbol, symbol }, text);
            m_search_rect = { m_search_rect.x, 0, m_search_rect.width, height() };
        }
        float const leftmost = m_focus_rect.is_empty() ? m_search_rect.x : m_focus_rect.x;
        // Programs' own menu extras go to the left of these.
        int const extras_edge = static_cast<int>(leftmost);
        if (extras_edge != m_extras_edge && m_tells_extras) {
            m_extras_edge = extras_edge;
            (void)Application::the().connection().server().set_menu_extras_edge(extras_edge, static_cast<int>(t.metric("menubar.statusGap"_sv)) - 8);
        }
    }

    bool mouse_down(MouseEvent const& event) override
    {
        if (event.button != WS::ButtonLeft)
            return false;
        if (m_clock_rect.contains(event.position)) {
            if (on_panel_opening)
                on_panel_opening();
            m_center.toggle_center();
            return false;
        }
        if (m_control_rect.contains(event.position)) {
            if (on_panel_opening)
                on_panel_opening();
            m_control.toggle();
            return false;
        }
        if (m_search_rect.contains(event.position)) {
            m_search_panel.toggle();
            return false;
        }
        layout_items();
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (event.position.x >= m_items[i].left && event.position.x < m_items[i].left + m_items[i].width) {
                open_item(static_cast<int>(i));
                return false;
            }
        }
        return false;
    }

    void open_item(int index)
    {
        if (index == m_open)
            return;
        close_open_menu();
        auto const& item = m_items[static_cast<size_t>(index)];
        m_open = index;
        set_needs_display();
        if (item.is_help) {
            auto const origin = window()->frame().origin();
            auto const pid = m_state.active_pid;
            m_help.open({ origin.x + static_cast<int>(item.left), origin.y + static_cast<int>(height()) + 1 }, [pid](u32 id) { (void)Application::the().connection().server().choose_menu_item(pid, id); }, [this] {
                m_open = -1;
                set_needs_display();
            });
            return;
        }
        auto* menu = item.menu;
        if (!menu)
            return;
        menu->on_close = [this, menu] {
            // (A switch to another menu closes this one first.)
            if (m_open >= 0 && m_items[static_cast<size_t>(m_open)].menu == menu) {
                m_open = -1;
                set_needs_display();
            }
        };
        menu->on_pointer_outside = [this](FloatPoint screen) {
            if (screen.y >= height())
                return;
            layout_items();
            for (size_t i = 0; i < m_items.size(); ++i) {
                if (screen.x >= m_items[i].left && screen.x < m_items[i].left + m_items[i].width) {
                    auto target = static_cast<int>(i);
                    if (target != m_open)
                        Platform::the().defer([this, target] { open_item(target); });
                    return;
                }
            }
        };
        float bar = tokens().metric("menubar.height"_sv);
        menu->open(*window(), { static_cast<int>(item.left), static_cast<int>(bar) });
    }

private:
    struct Item {
        String title;
        float left { 0 };
        float width { 0 };
        Menu* menu { nullptr };
        bool bold { false };
        bool is_logo { false };
        // Every application's last: searched, rather than a list.
        bool is_help { false };
    };

    void close_open_menu()
    {
        m_help.close();
        if (m_open < 0)
            return;
        auto* menu = m_items[static_cast<size_t>(m_open)].menu;
        m_open = -1;
        if (menu)
            menu->close();
    }

    void rebuild_menus()
    {
        close_open_menu();
        m_items.clear();
        m_menus.clear();
        auto add_item = [&](StringView title, Menu* menu, bool bold, bool logo) {
            Item item;
            item.title = MUST(String::from_view(title));
            item.menu = menu;
            item.bold = bold;
            item.is_logo = logo;
            m_items.append(move(item));
        };
        add_item({}, m_system_menu.ptr(), false, true);
        auto pid = m_state.active_pid;
        m_choose = [pid](u32 id) { (void)Application::the().connection().server().choose_menu_item(pid, id); };
        auto json = JSON::parse(m_state.menus.view());
        bool any = !json.is_error() && json.value().is_array() && json.value().size() > 0;
        if (any) {
            for (size_t i = 0; i < json.value().size(); ++i) {
                auto const& description = json.value().at(i);
                m_menus.append(Desktop::menu_from_json(description, m_choose));
            }
            for (size_t i = 0; i < json.value().size(); ++i) {
                auto const& description = json.value().at(i);
                auto title = description.get_string("title"_sv).value_or(StringView {});
                add_item(i == 0 ? (m_state.active_name.is_empty() ? title : m_state.active_name.view()) : title, m_menus[i].ptr(), i == 0, false);
            }
            // Help, at the end, as every Mac application has it: it finds the others' commands.
            m_help.set_menus(json.value());
            add_item("Help"_sv, nullptr, false, false);
            m_items.last().is_help = true;
        } else if (!m_state.active_name.is_empty()) {
            add_item(m_state.active_name.view(), nullptr, true, false);
        }
    }

    void layout_items()
    {
        auto const& t = tokens();
        float x = t.metric("menubar.leftPadding"_sv);
        float padding = t.metric("menubar.itemPadding"_sv);
        auto font = t.named_font("menu"_sv);
        auto bold = t.named_font("headline"_sv);
        for (auto& item : m_items) {
            item.left = x;
            if (item.is_logo)
                item.width = t.metric("menubar.logoSize"_sv) + padding * 2;
            else
                item.width = Appearance::text_width(item.bold ? bold : font, item.title.view()) + padding * 2;
            x += item.width;
        }
    }

    SearchPanel& m_search_panel;
    DesktopShell::Notifications& m_center;
    QuickSettingsPanel& m_control;
    Desktop::ShellState m_state;
    OwnPtr<Menu> m_system_menu;
    Vector<NonnullOwnPtr<Menu>> m_menus;
    Function<void(u32)> m_choose;
    Vector<Item> m_items;
    int m_open { -1 };
    String m_clock;
    FloatRect m_clock_rect;
    FloatRect m_control_rect;
    FloatRect m_search_rect;
    FloatRect m_focus_rect;
    int m_extras_edge { -1 };
    DesktopShell::HelpSearch m_help;

public:
    // Only the main display's says where menu extras go.
    bool m_tells_extras { false };
    // Control Center's setting: the moon while a Focus is on, or not.
    bool m_shows_focus { true };
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
        fprintf(stderr, "DesktopShell: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    if (s_still)
        set_motion_enabled(false);
    auto& server = application->connection().server();

    Desktop::Preferences preferences("com.vulture.desktop"_sv);
    if (auto saved = preferences.string("appearance"_sv); saved.has_value())
        (void)server.set_appearance(saved.value());
    if (preferences.boolean("reduceMotion"_sv).value_or(false))
        (void)server.set_reduce_motion(true);
    if (auto saved = preferences.boolean("naturalScrolling"_sv); saved.has_value())
        (void)server.set_natural_scrolling(saved.value());
    if (auto saved = preferences.boolean("tapToClick"_sv); saved.has_value())
        (void)server.set_tap_to_click(saved.value());
    if (auto saved = preferences.number("pointerSpeed"_sv); saved.has_value())
        (void)server.set_pointer_speed(static_cast<i32>(saved.value()));
    if (auto saved = preferences.number("nightShift"_sv); saved.has_value() && saved.value() > 0)
        (void)server.set_warmth(static_cast<i32>(saved.value()));
    (void)server.set_tiling(preferences.boolean("tileByEdges"_sv).value_or(true), preferences.boolean("tileToMenuBar"_sv).value_or(true), preferences.boolean("tileWithOption"_sv).value_or(true), preferences.boolean("tileMargins"_sv).value_or(true));
    (void)server.set_click_wallpaper(preferences.boolean("clickWallpaperReveals"_sv).value_or(true));
    (void)server.set_hot_corners(static_cast<i32>(preferences.number("hotCornerTopLeft"_sv).value_or(0)), static_cast<i32>(preferences.number("hotCornerTopRight"_sv).value_or(0)), static_cast<i32>(preferences.number("hotCornerBottomLeft"_sv).value_or(0)), static_cast<i32>(preferences.number("hotCornerBottomRight"_sv).value_or(0)));
    if (preferences.boolean("stage"_sv).value_or(false))
        (void)server.set_stage(true);
    if (preferences.boolean("mirrorDisplays"_sv).value_or(false))
        (void)server.set_mirroring(true);
    // Where each display goes, and how big it's to look: "output x y width height" and so on.
    if (auto saved = preferences.string("displayLayout"_sv); saved.has_value()) {
        int numbers[5] = {};
        int which = 0;
        bool negative = false;
        int value = 0;
        bool in_number = false;
        auto const finish_number = [&] {
            if (!in_number)
                return;
            numbers[which++] = negative ? -value : value;
            value = 0;
            negative = false;
            in_number = false;
            if (which == 5) {
                (void)server.set_display(numbers[0], numbers[1], numbers[2], numbers[3], numbers[4]);
                which = 0;
            }
        };
        for (auto const character : saved.value()) {
            if (character == '-') {
                negative = true;
            } else if (character >= '0' && character <= '9') {
                value = value * 10 + (character - '0');
                in_number = true;
            } else {
                finish_number();
            }
        }
        finish_number();
    }
    if (auto saved = preferences.string("wallpaper"_sv); saved.has_value())
        (void)server.set_wallpaper(saved.value());
    if (auto saved = preferences.string("keyboardLayout"_sv); saved.has_value())
        (void)server.set_keyboard_layout(saved.value());

    SearchPanel search_panel;
    // Widgets, on the desktop and in Notification Center, and notifications.
    DesktopShell::set_widgets_still(s_still);
    DesktopShell::set_notifications_still(s_still);
    DesktopShell::Widgets widgets;
    DesktopShell::Notifications center(widgets);
    QuickSettingsPanel control;
    center.is_quiet = [&control] { return control.is_quiet(); };

    auto screen = Platform::the().screen_size();
    auto bar_height = static_cast<int>(tokens().metric("menubar.height"_sv));
    auto window = adopt_own(*new UI::Window("Menu Bar"_sv, { 0, 0, screen.width, bar_height }, WS::WindowBare | WS::WindowSticky));
    window->set_transparent(true);
    window->backend().set_level(WS::LevelMenuBar);
    MaterialRegion material { { 0, 0, screen.width, bar_height }, WS::Material::MenuBar };
    window->set_materials(Span<MaterialRegion const>(&material, 1));
    auto& bar = window->root_view().add<MenuBarView>(search_panel, center, control);
    bar.m_tells_extras = true;
    DesktopShell::AboutWindow about;
    DesktopShell::ForceQuitWindow force_quit;
    bar.set_frame({ 0, 0, static_cast<float>(screen.width), static_cast<float>(bar_height) });
    bar.tick_clock();
    window->show();
    (void)server.set_work_area(bar_height, 0, 0, 0);

    // One on each of the others too, the same but dimmer, as it's not where the work's going on.
    struct OtherBar {
        int output;
        OwnPtr<UI::Window> window;
        MenuBarView* view;
    };
    Vector<OtherBar> other_bars;
    String last_state;
    auto place_bars = [&] {
        auto const displays = application->connection().displays();
        // The main one keeps up with its own display.
        auto const main_size = Platform::the().screen_size();
        if (window->frame().width != main_size.width) {
            window->set_frame({ 0, 0, main_size.width, bar_height });
            MaterialRegion resized { { 0, 0, main_size.width, bar_height }, WS::Material::MenuBar };
            window->set_materials(Span<MaterialRegion const>(&resized, 1));
            bar.set_frame({ 0, 0, static_cast<float>(main_size.width), static_cast<float>(bar_height) });
        }
        other_bars.remove_all_matching([&](auto const& other) {
            for (auto const& display : displays) {
                if (display.output == other.output && !(display.x == 0 && display.y == 0))
                    return false;
            }
            return true;
        });
        for (auto const& display : displays) {
            if (display.x == 0 && display.y == 0)
                continue;
            OtherBar* found = nullptr;
            for (auto& other : other_bars) {
                if (other.output == display.output)
                    found = &other;
            }
            IntRect const frame { display.x, display.y, display.width, bar_height };
            if (!found) {
                auto other_window = adopt_own(*new UI::Window("Menu Bar"_sv, frame, WS::WindowBare | WS::WindowSticky));
                other_window->set_transparent(true);
                other_window->backend().set_level(WS::LevelMenuBar);
                auto& view = other_window->root_view().add<MenuBarView>(search_panel, center, control);
                view.set_opacity(0.55f);
                view.tick_clock();
                other_window->show();
                other_bars.append(OtherBar { display.output, move(other_window), &view });
                found = &other_bars.last();
                auto state = Desktop::parse_shell_state(last_state.view());
                if (!state.is_error())
                    found->view->set_state(state.release_value());
            }
            found->window->set_frame(frame);
            MaterialRegion material_there { { 0, 0, display.width, bar_height }, WS::Material::MenuBar };
            found->window->set_materials(Span<MaterialRegion const>(&material_there, 1));
            found->view->set_frame({ 0, 0, static_cast<float>(display.width), static_cast<float>(bar_height) });
        }
    };
    application->connection().on_displays_change = [&] { place_bars(); };

    // The green button's menu, for whichever window's asking: they don't all have a menu bar, and it's not the program's business.
    OwnPtr<Menu> window_menu;
    application->connection().on_window_menu_wanted = [&](u32 target, IntPoint at) {
        window_menu = adopt_own(*new Menu);
        auto arranging = [&server, target](WS::WindowArrangement arrangement) {
            return [&server, target, arrangement] { (void)server.arrange_window(target, static_cast<i32>(arrangement)); };
        };
        // Sequoia's: rows of pictures under headings, Option changing some of them.
        auto picture = [&](StringView title, WS::WindowArrangement arrangement) -> MenuItem& {
            return window_menu->add_picture(title, picture_of_arrangement(arrangement), arranging(arrangement));
        };
        auto alternate = [&](MenuItem& item, StringView title, WS::WindowArrangement arrangement) {
            Menu::add_alternate(item, title, arranging(arrangement)).picture = picture_of_arrangement(arrangement);
        };
        (void)window_menu->add_heading("Move & Resize"_sv);
        (void)picture("Left"_sv, WS::WindowArrangement::Left);
        (void)picture("Right"_sv, WS::WindowArrangement::Right);
        (void)picture("Top"_sv, WS::WindowArrangement::Top);
        (void)picture("Bottom"_sv, WS::WindowArrangement::Bottom);
        (void)window_menu->add_heading("Fill & Arrange"_sv);
        alternate(picture("Fill"_sv, WS::WindowArrangement::Fill), "Center"_sv, WS::WindowArrangement::Center);
        alternate(picture("Left & Right"_sv, WS::WindowArrangement::LeftAndRight), "Right & Left"_sv, WS::WindowArrangement::RightAndLeft);
        alternate(picture("Left & Quarters"_sv, WS::WindowArrangement::LeftAndQuarters), "Right & Quarters"_sv, WS::WindowArrangement::RightAndQuarters);
        (void)picture("Quarters"_sv, WS::WindowArrangement::Quarters);
        (void)window_menu->add_heading("Full Screen Tile"_sv);
        (void)window_menu->add_picture("Entire Screen"_sv, picture_of_arrangement(WS::WindowArrangement::Fill), [&server, target] { (void)server.set_full_screen(target, true); });
        (void)window_menu->add_picture("Left of Screen"_sv, picture_of_split_view(true), [&server, target] { (void)server.tile_in_split_view(target, true); });
        (void)window_menu->add_picture("Right of Screen"_sv, picture_of_split_view(false), [&server, target] { (void)server.tile_in_split_view(target, false); });
        window_menu->open(*window, at);
    };
    (void)server.show_window_menus();
    application->connection().on_hot_corner = [&](WS::HotCornerAction action) {
        if (action == WS::HotCornerAction::NotificationCenter)
            center.toggle_center();
        else if (action == WS::HotCornerAction::Launcher)
            (void)Desktop::open_path("/System/Applications/Launcher.app"_sv);
    };

    Switcher switcher;
    application->connection().on_shell_state = [&](StringView json) {
        last_state = MUST(String::from_view(json));
        auto state = Desktop::parse_shell_state(json);
        if (!state.is_error()) {
            switcher.update(state.value());
            force_quit.set_state(state.value());
            // The desktop is what's in front when it's been clicked, or the Files that owns it has no window: widgets on it
            // in full colour.
            bool desktop_active = state.value().desktop_revealed;
            if (state.value().active_identifier.view() == "com.vulture.Files"_sv) {
                bool any = false;
                for (auto const& entry : state.value().applications) {
                    if (entry.pid == state.value().active_pid && !entry.hidden) {
                        for (auto const& one : entry.windows)
                            any = any || (!one.minimized && one.space == state.value().space);
                    }
                }
                desktop_active = desktop_active || !any;
            }
            widgets.set_desktop_active(desktop_active);
            bar.set_state(state.release_value());
        }
        for (auto& other : other_bars) {
            auto again = Desktop::parse_shell_state(json);
            if (!again.is_error())
                other.view->set_state(again.release_value());
        }
    };
    place_bars();
    (void)server.subscribe_shell();
    // Shift-Command-3, 4 and 5.
    DesktopShell::set_screenshots_still(s_still);
    DesktopShell::Screenshots screenshots;
    screenshots.register_keys();
    // Focus, from Control Center or Settings: said in the menu bar.
    control.on_focus_change = [&bar] { bar.set_needs_display(); };
    // Menu extras: programs of their own, started as Control Center's settings say. (Sound's, for now.)
    pid_t sound_menu = 0;
    auto update_menu_extras = [&sound_menu, &bar] {
        Desktop::Preferences control_center("com.vulture.controlcenter"_sv);
        bar.m_shows_focus = control_center.string("focus"_sv).value_or("whenActive"_sv) != "never"_sv;
        bar.set_needs_display();
        bool const wanted = control_center.string("sound"_sv).value_or("whenActive"_sv) == "always"_sv;
        if (wanted && sound_menu == 0) {
            pid_t const child = fork();
            if (child == 0) {
                execl("/System/Library/CoreServices/SoundMenu", "/System/Library/CoreServices/SoundMenu", nullptr);
                _exit(127);
            }
            if (child > 0)
                sound_menu = child;
        } else if (!wanted && sound_menu != 0) {
            (void)kill(sound_menu, SIGTERM);
            sound_menu = 0;
        }
    };
    update_menu_extras();
    auto listen = [&](char const* name, Function<void()> heard) {
        int fd = -1;
        int token = -1;
        if (notify_register_file_descriptor(name, &fd, 0, &token) != NOTIFY_STATUS_OK)
            return;
        auto shared = adopt_own(*new Function<void()>(move(heard)));
        auto* raw = shared.leak_ptr();
        (void)application->event_loop().watch(fd, Core::EventLoop::Interest::Read, [fd, raw] {
            u8 bytes[4];
            (void)read(fd, bytes, sizeof(bytes));
            (*raw)();
        });
    };
    listen("com.vulture.controlcenter.preferences", [&update_menu_extras] { update_menu_extras(); });
    listen("com.vulture.focus.preferences", [&control] { control.reload_focus(); });

    // About This Computer, and Force Quit (Command-Option-Escape too).
    bar.on_about = [&about] { about.show(); };
    bar.on_force_quit = [&force_quit] { force_quit.show(); };
    (void)server.register_hotkey(KeyEscape, WS::ModifierCommand | WS::ModifierOption);
    application->connection().on_hotkey = [&search_panel, &screenshots, &force_quit](u16 code, u32 modifiers) {
        if ((modifiers & WS::ModifierShift) && screenshots.hotkey(code, modifiers))
            return;
        if (code == KeyEscape && (modifiers & WS::ModifierOption)) {
            force_quit.show();
            return;
        }
        if (code == KeySpace)
            search_panel.toggle();
    };
    (void)server.register_hotkey(KeySpace, WS::ModifierCommand);
    if (!s_still)
        application->event_loop().add_timer(1000, true, [&] {
            bar.tick_clock();
            for (auto& other : other_bars)
                other.view->tick_clock();
        });

    bar.on_panel_opening = [&center] { center.hide_banners(); };
    ConsentPanel consent;
    WorkspaceService workspace;
    printf("desktopshell: ready\n");
    fflush(stdout);
    return application->exec();
}
