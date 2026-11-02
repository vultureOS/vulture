/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "LogLine.h"
#include <Desktop.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/TableView.h>
#include <LibUI/TextField.h>
#include <LogEndpoint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

// The unified log, as it happens: what logd has, then every new entry as it arrives.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

bool contains_ignoring_case(StringView text, StringView part)
{
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    if (part.length() > text.length())
        return false;
    for (size_t i = 0; i + part.length() <= text.length(); ++i) {
        size_t j = 0;
        while (j < part.length() && lower(text[i + j]) == lower(part[j]))
            ++j;
        if (j == part.length())
            return true;
    }
    return false;
}

class LogModel final : public TableModel {
public:
    [[nodiscard]] size_t row_count() const override { return m_shown.size(); }

    [[nodiscard]] String text_at(size_t row, size_t column) const override
    {
        Console::LogLine line;
        auto const& text = m_lines[m_shown[row]];
        if (!Console::parse_log_line(text.view(), line))
            return column == 3 ? MUST(String::from_view(text.view())) : String {};
        switch (column) {
        case 0:
            return MUST(String::from_view(line.time));
        case 1:
            return MUST(String::from_view(line.type));
        case 2:
            return MUST(String::from_view(line.process));
        case 3:
            return MUST(String::from_view(line.message));
        default:
            return {};
        }
    }

    // False if it is filtered out, or the header `log` prints.
    bool add(StringView line)
    {
        // The oldest go, past the limit.
        size_t limit = static_cast<size_t>(tokens().metric("console.limit"_sv));
        if (m_lines.size() >= limit) {
            m_lines.remove(0);
            rebuild();
        }
        m_lines.append(MUST(String::from_view(line)));
        if (matches(line)) {
            m_shown.append(m_lines.size() - 1);
            return true;
        }
        return false;
    }

    void clear()
    {
        m_lines.clear();
        m_shown.clear();
    }

    void set_filter(StringView text)
    {
        m_filter = MUST(String::from_view(text));
        rebuild();
    }

    [[nodiscard]] size_t total() const { return m_lines.size(); }

private:
    [[nodiscard]] bool matches(StringView line) const
    {
        return m_filter.is_empty() || contains_ignoring_case(line, m_filter.view());
    }

    void rebuild()
    {
        m_shown.clear();
        for (size_t i = 0; i < m_lines.size(); ++i) {
            if (matches(m_lines[i].view()))
                m_shown.append(i);
        }
    }

    Vector<String> m_lines;
    Vector<size_t> m_shown;
    String m_filter;
};

class ConsoleApplication;

class Stream final : public LogStreamStub {
public:
    explicit Stream(ConsoleApplication& owner)
        : m_owner(owner)
    {
    }

    void entry(IPC::Sender const&, String line) override;

private:
    ConsoleApplication& m_owner;
};

class ConsoleApplication {
public:
    ConsoleApplication()
        : m_stream(*this)
    {
        auto const& t = tokens();
        int width = static_cast<int>(t.metric("console.width"_sv));
        int height = static_cast<int>(t.metric("console.height"_sv));
        auto& window = Application::the().make_window<UI::Window>("Console"_sv, IntRect { 0, 0, width, height }, WS::WindowTitled | WS::WindowResizable | WS::WindowUnifiedTitle);
        window.center_on_screen();
        window.set_sheet_top(t.metric("window.toolbarUnified"_sv));
        auto& stack = window.set_content<StackView>(Axis::Vertical);
        auto& toolbar = stack.add<Toolbar>();
        toolbar.set_title("Console"_sv);
        auto& levels = toolbar.add_segmented();
        levels.add_segment("Default"_sv);
        levels.add_segment("Info"_sv);
        levels.add_segment("Debug"_sv);
        levels.set_selected(0);
        levels.on_change = [this](size_t level) { connect(static_cast<u8>(level)); };
        (void)toolbar.add_button(Symbol::ClearCircle, [this] {
            m_model.clear();
            changed(false);
        },
            "Clear"_sv);
        toolbar.add_flexible_space();
        auto& search = toolbar.add_search(220);
        search.on_change = [this, &search] {
            m_model.set_filter(search.text());
            changed(false);
        };

        auto& table = stack.add<TableView>(&m_model);
        m_table = &table;
        table.set_stretch(1, 1);
        table.set_alternating_rows(true);
        table.add_column("Time"_sv, 140);
        table.add_column("Type"_sv, 76);
        table.add_column("Process"_sv, 130);
        table.add_column("Message"_sv, 534);

        auto& status_row = stack.add<StackView>(Axis::Horizontal, 8, Alignment::Center, Insets { 0, 12, 0, 12 });
        status_row.set_fixed_height(t.metric("console.statusHeight"_sv));
        m_status = &status_row.add<Label>(StringView {});
        m_status->set_style(LabelStyle::Secondary);
        m_status->set_stretch(1, 0);

        window.show();
        (void)window.make_first_responder(&table);
        connect(0);
    }

    void line_arrived(StringView line)
    {
        bool shown = m_model.add(line);
        // Following along, unless a row is being looked at.
        changed(shown && m_table->selected_row() < 0);
    }

private:
    void connect(u8 level)
    {
        m_listener = nullptr;
        m_model.clear();
        auto port = IPC::look_up("com.vulture.logd"_sv);
        if (port.is_error()) {
            m_table->reload();
            m_status->set_text("The log service cannot be reached."_sv);
            return;
        }
        LogProxy logd(port.release_value());
        auto earlier = logd.show(0, level, 0, StringView {});
        if (!earlier.is_error()) {
            auto const& lines = earlier.value().lines;
            for (size_t i = 0; i < lines.size(); ++i)
                (void)m_model.add(lines[i].view());
        }
        auto sink = IPC::create_port();
        if (!sink.is_error()) {
            auto accepted = logd.stream(move(sink.value().send), level, 0, StringView {});
            if (!accepted.is_error() && accepted.value()) {
                m_listener = adopt_own(*new IPC::Listener(Application::the().event_loop(), move(sink.value().receive), [this](IPC::ReceivedMessage& message) {
                    (void)m_stream.handle(message);
                }));
            }
        }
        changed(true);
    }

    void changed(bool follow)
    {
        m_table->reload();
        if (follow && m_model.row_count() > 0)
            m_table->scroll_to_row(m_model.row_count() - 1);
        StringBuilder status;
        status.appendff("{} of {} messages", m_model.row_count(), m_model.total());
        m_status->set_text(status.view());
    }

    LogModel m_model;
    Stream m_stream;
    TableView* m_table { nullptr };
    Label* m_status { nullptr };
    OwnPtr<IPC::Listener> m_listener;
};

void Stream::entry(IPC::Sender const&, String line)
{
    m_owner.line_arrived(line.view());
}

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto application_or_error = Application::create("Console"_sv, "com.vulture.Console"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Console: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    application->set_main_menu(application->standard_menus());
    ConsoleApplication console;
    printf("console: ready\n");
    fflush(stdout);
    return application->exec();
}
