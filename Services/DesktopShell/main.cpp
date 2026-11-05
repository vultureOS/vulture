/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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

// ---- Notifications

class Banner final : public View {
public:
    void set_note(StringView application, StringView title, StringView body)
    {
        m_application = MUST(String::from_view(application));
        m_title = MUST(String::from_view(title));
        m_body = MUST(String::from_view(body));
        set_needs_display();
    }

    Function<void()> on_click;

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float radius = t.metric("notifications.radius"_sv);
        graphics.fill_rounded({ 0, 0, width(), height() }, radius, t.color("notifications.fill"_sv));
        float icon = t.metric("notifications.iconSize"_sv);
        auto picture = Desktop::icon_named("Settings"_sv, static_cast<int>(icon), static_cast<int>(graphics.scale()));
        if (picture)
            graphics.painter().draw_bitmap(*picture, FloatRect { 12, (height() - icon) / 2, icon, icon });
        float x = 12 + icon + 10;
        auto bold = t.named_font("headline"_sv);
        auto body = t.named_font("callout"_sv);
        graphics.draw_text_truncated(m_title.view(), bold, { x, 28 }, width() - x - 12, t.color("label"_sv));
        graphics.draw_text_truncated(m_body.view(), body, { x, 46 }, width() - x - 12, t.color("notifications.secondary"_sv));
    }

    bool mouse_down(MouseEvent const&) override
    {
        if (on_click)
            on_click();
        return true;
    }

private:
    String m_application;
    String m_title;
    String m_body;
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

class NotificationListener final : public NotificationEventsStub {
public:
    Function<void(StringView, StringView, StringView)> on_posted;

    void posted(IPC::Sender const&, String application, String title, String body) override
    {
        if (on_posted)
            on_posted(application.view(), title.view(), body.view());
    }
};

class NotificationServicePanel {
public:
    NotificationServicePanel()
    {
        auto const& t = tokens();
        auto screen = Platform::the().screen_size();
        auto width = static_cast<int>(t.metric("notifications.centerWidth"_sv));
        float bar = t.metric("menubar.height"_sv);
        auto top = static_cast<int>(bar) + 4;
        m_window = adopt_own(*new UI::Window("Notifications"_sv, { screen.width - width - 8, top, width, screen.height - top - 8 }, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
        m_window->set_transparent(true);
        m_window->backend().set_level(WS::LevelPopup);
        auto& content = m_window->root_view().add<Content>();
        content.set_frame({ 0, 0, static_cast<float>(width), static_cast<float>(screen.height - top - 8) });
        m_content = &content;
        MaterialRegion material { { 0, 0, width, screen.height - top - 8 }, WS::Material::Popover, static_cast<int>(t.metric("notifications.radius"_sv)) };
        m_window->set_materials(Span<MaterialRegion const>(&material, 1));
        m_window->on_key_change = [this](bool active) {
            if (!active && m_shown) {
                m_shown = false;
                m_window->hide();
            }
        };
    }

    void toggle()
    {
        if (m_shown) {
            m_shown = false;
            m_window->hide();
            return;
        }
        m_shown = true;
        reload();
        m_window->show();
    }

    void reload()
    {
        m_content->notes.clear();
        auto port = IPC::look_up("com.vulture.notificationd"_sv);
        if (port.is_error())
            return;
        NotificationServiceProxy center(port.release_value());
        auto answer = center.all();
        if (answer.is_error())
            return;
        auto json = JSON::parse(answer.value().view());
        if (json.is_error())
            return;
        for (size_t i = 0; i < json.value().size(); ++i) {
            auto const& one = json.value().at(i);
            m_content->notes.append(Note { MUST(String::from_view(one.get_string("title"_sv).value_or(StringView {}))), MUST(String::from_view(one.get_string("body"_sv).value_or(StringView {}))) });
        }
        m_content->set_needs_display();
    }

private:
    struct Note {
        String title;
        String body;
    };

    class Content final : public View {
    public:
        void draw(Graphics& graphics) override
        {
            auto const& t = tokens();
            graphics.fill_rounded({ 0, 0, width(), height() }, t.metric("notifications.radius"_sv), t.color("notifications.center"_sv));
            auto date = clock_text();
            FontSpec big { t.metric("notifications.dateSize"_sv), FontWeight::Medium, false };
            graphics.draw_text(date.view(), big, { 16, 40 }, t.color("label"_sv));
            float y = 60;
            auto bold = t.named_font("headline"_sv);
            auto small = t.named_font("callout"_sv);
            if (notes.is_empty()) {
                graphics.draw_text("No Notifications"_sv, t.named_font("body"_sv), { 16, y + 20 }, t.color("secondaryLabel"_sv));
                return;
            }
            float card = t.metric("notifications.height"_sv);
            for (auto const& note : notes) {
                FloatRect place { 8, y, width() - 16, card };
                graphics.fill_rounded(place, t.metric("notifications.radius"_sv), t.color("notifications.fill"_sv));
                graphics.draw_text_truncated(note.title.view(), bold, { place.x + 14, y + 28 }, place.width - 28, t.color("label"_sv));
                graphics.draw_text_truncated(note.body.view(), small, { place.x + 14, y + 46 }, place.width - 28, t.color("notifications.secondary"_sv));
                y += card + 8;
                if (y > height())
                    break;
            }
        }

        Vector<Note> notes;
    };

    OwnPtr<UI::Window> m_window;
    Content* m_content { nullptr };
    bool m_shown { false };
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
        auto& quiet = stack.add<Tile>("Do Not Disturb"_sv, Symbol::Minus, false);
        quiet.set_fixed_height(tile_height);
        m_quiet = &quiet;
        quiet.on_action = [&quiet] { quiet.set_on(!quiet.is_on()); };
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

    MenuBarView(SearchPanel& search_panel, NotificationServicePanel& center, QuickSettingsPanel& control)
        : m_search_panel(search_panel)
        , m_center(center)
        , m_control(control)
    {
        m_system_menu = adopt_own(*new Menu);
        m_system_menu->add("About This Computer"_sv, [] {
            auto bundle = Desktop::read_bundle("/System/Applications/Settings.app"_sv);
            StringView arguments[1] = { "About"_sv };
            if (!bundle.is_error())
                (void)Desktop::launch(bundle.value(), Span<StringView const>(arguments, 1));
        });
        m_system_menu->add_separator();
        m_system_menu->add("Settings…"_sv, [] { (void)Desktop::open_path("/System/Applications/Settings.app"_sv); });
        m_system_menu->add_separator();
        m_system_menu->add("Restart…"_sv, [] { Desktop::restart(); });
        m_system_menu->add("Shut Down…"_sv, [] { Desktop::shut_down(); });
        m_system_menu->add_separator();
        m_system_menu->add("Log Out"_sv, [] { Desktop::log_out(); });
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
    }

    bool mouse_down(MouseEvent const& event) override
    {
        if (event.button != WS::ButtonLeft)
            return false;
        if (m_clock_rect.contains(event.position)) {
            if (on_panel_opening)
                on_panel_opening();
            m_center.toggle();
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
    };

    void close_open_menu()
    {
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
    NotificationServicePanel& m_center;
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
    NotificationServicePanel center;
    QuickSettingsPanel control;

    auto screen = Platform::the().screen_size();
    auto bar_height = static_cast<int>(tokens().metric("menubar.height"_sv));
    auto window = adopt_own(*new UI::Window("Menu Bar"_sv, { 0, 0, screen.width, bar_height }, WS::WindowBare | WS::WindowSticky));
    window->set_transparent(true);
    window->backend().set_level(WS::LevelMenuBar);
    MaterialRegion material { { 0, 0, screen.width, bar_height }, WS::Material::MenuBar };
    window->set_materials(Span<MaterialRegion const>(&material, 1));
    auto& bar = window->root_view().add<MenuBarView>(search_panel, center, control);
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

    application->connection().on_shell_state = [&](StringView json) {
        last_state = MUST(String::from_view(json));
        auto state = Desktop::parse_shell_state(json);
        if (!state.is_error())
            bar.set_state(state.release_value());
        for (auto& other : other_bars) {
            auto again = Desktop::parse_shell_state(json);
            if (!again.is_error())
                other.view->set_state(again.release_value());
        }
    };
    place_bars();
    (void)server.subscribe_shell();
    application->connection().on_hotkey = [&search_panel](u16 code, u32) {
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

    // Banners.
    auto const& t = tokens();
    auto banner_width = static_cast<int>(t.metric("notifications.width"_sv));
    auto banner_height = static_cast<int>(t.metric("notifications.height"_sv));
    auto margin = static_cast<int>(t.metric("notifications.margin"_sv));
    auto banner_window = adopt_own(*new UI::Window("Notification"_sv, { screen.width - banner_width - margin, bar_height + margin, banner_width, banner_height }, WS::WindowBare | WS::WindowPopup | WS::WindowSticky));
    banner_window->set_transparent(true);
    MaterialRegion banner_material { { 0, 0, banner_width, banner_height }, WS::Material::Popover, static_cast<int>(t.metric("notifications.radius"_sv)) };
    banner_window->set_materials(Span<MaterialRegion const>(&banner_material, 1));
    auto& banner = banner_window->root_view().add<Banner>();
    banner.set_frame({ 0, 0, static_cast<float>(banner_width), static_cast<float>(banner_height) });
    int banner_timer = -1;
    banner.on_click = [&] {
        banner_window->hide();
        center.toggle();
    };
    bar.on_panel_opening = [&] { banner_window->hide(); };
    NotificationListener listener;
    listener.on_posted = [&](StringView application_name, StringView title, StringView body) {
        if (control.is_quiet())
            return;
        banner.set_note(application_name, title, body);
        banner_window->show();
        application->play_sound("notification"_sv);
        printf("notification: %.*s\n", static_cast<int>(title.length()), title.characters_without_null_termination());
        fflush(stdout);
        if (banner_timer >= 0)
            application->cancel_timer(banner_timer);
        // (When nothing moves, it stays until something makes it go.)
        if (motion_enabled()) {
            auto seconds = static_cast<u64>(tokens().metric("notifications.seconds"_sv));
            banner_timer = application->add_timer(seconds * 1000, false, [&] {
                banner_timer = -1;
                banner_window->hide();
            });
        }
    };
    int events[2];
    OwnPtr<IPC::Listener> notifications_listener;
    auto center_port = IPC::look_up("com.vulture.notificationd"_sv);
    if (!center_port.is_error()) {
        auto pair = IPC::create_port();
        if (!pair.is_error()) {
            auto ports = pair.release_value();
            notifications_listener = adopt_own(*new IPC::Listener(application->event_loop(), move(ports.receive), [&](IPC::ReceivedMessage& message) { (void)listener.handle(message); }));
            (void)NotificationServiceProxy(center_port.release_value()).subscribe(move(ports.send));
        }
    }
    (void)events;
    ConsentPanel consent;
    WorkspaceService workspace;
    printf("desktopshell: ready\n");
    fflush(stdout);
    return application->exec();
}
