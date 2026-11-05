/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <Kernel/API/POSIX/sysctl.h>
#include <LibBase/HashMap.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/Panels.h>
#include <LibUI/TableView.h>
#include <LibDesktop/Desktop.h>
#include <LibUI/TextField.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

// Every process, what it is using, and a way to end it. Sampled from "kern.proc.all":
// a process's share of the processors is how much CPU time it gained since the last
// sample, out of the time that went by (100% is one processor, flat out).

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

double now_seconds()
{
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

template<typename T>
T system_value(char const* name, T fallback = 0)
{
    T value = fallback;
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0)
        return fallback;
    return value;
}

String format_bytes(u64 bytes)
{
    StringBuilder builder;
    auto tenths = [&](u64 value, u64 unit, StringView name) {
        u64 scaled = (value * 10 + unit / 2) / unit;
        builder.appendff("{}.{} ", scaled / 10, scaled % 10);
        builder.append(name);
    };
    if (bytes >= 1024ull * 1024 * 1024)
        tenths(bytes, 1024ull * 1024 * 1024, "GB"_sv);
    else if (bytes >= 1024ull * 1024)
        tenths(bytes, 1024ull * 1024, "MB"_sv);
    else if (bytes >= 1024)
        tenths(bytes, 1024, "KB"_sv);
    else
        builder.appendff("{} bytes", bytes);
    return MUST(builder.to_string());
}

String format_cpu_time(u64 nanoseconds)
{
    u64 hundredths = nanoseconds / 10'000'000;
    u64 seconds = hundredths / 100;
    StringBuilder builder;
    if (seconds >= 3600)
        builder.appendff("{}:{:02}:{:02}.{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60, hundredths % 100);
    else
        builder.appendff("{}:{:02}.{:02}", seconds / 60, seconds % 60, hundredths % 100);
    return MUST(builder.to_string());
}

String format_percent(double value)
{
    auto tenths = static_cast<u64>(value * 10 + 0.5);
    StringBuilder builder;
    builder.appendff("{}.{}", tenths / 10, tenths % 10);
    return MUST(builder.to_string());
}

struct Row {
    int pid { 0 };
    String name;
    double cpu { 0 };
    u64 cpu_nanoseconds { 0 };
    u64 threads { 0 };
    u64 memory { 0 };
    String user;
    char state { 'S' };
};

enum Column : size_t {
    ColumnName,
    ColumnCPU,
    ColumnCPUTime,
    ColumnThreads,
    ColumnMemory,
    ColumnPID,
    ColumnUser,
};

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

class ProcessModel final : public TableModel {
public:
    [[nodiscard]] size_t row_count() const override { return m_shown.size(); }

    [[nodiscard]] String text_at(size_t row, size_t column) const override
    {
        auto const& process = m_rows[m_shown[row]];
        switch (column) {
        case ColumnName: {
            for (auto const pid : m_not_responding) {
                if (pid != process.pid)
                    continue;
                StringBuilder name;
                name.appendff("{} (Not Responding)", process.name.view());
                return MUST(name.to_string());
            }
            return MUST(String::from_view(process.name.view()));
        }
        case ColumnCPU:
            return format_percent(process.cpu);
        case ColumnCPUTime:
            return format_cpu_time(process.cpu_nanoseconds);
        case ColumnThreads: {
            StringBuilder builder;
            builder.appendff("{}", process.threads);
            return MUST(builder.to_string());
        }
        case ColumnMemory:
            return format_bytes(process.memory);
        case ColumnPID: {
            StringBuilder builder;
            builder.appendff("{}", process.pid);
            return MUST(builder.to_string());
        }
        case ColumnUser:
            return MUST(String::from_view(process.user.view()));
        default:
            return {};
        }
    }

    [[nodiscard]] bool is_right_aligned(size_t column) const override
    {
        return column != ColumnName && column != ColumnUser;
    }

    void sort_by(size_t column, bool ascending) override
    {
        m_sort_column = column;
        m_sort_ascending = ascending;
        apply_sort();
    }

    void set_filter(StringView text)
    {
        m_filter = MUST(String::from_view(text));
        apply_filter();
    }

    [[nodiscard]] int pid_of(size_t row) const { return m_rows[m_shown[row]].pid; }
    [[nodiscard]] StringView name_of(size_t row) const { return m_rows[m_shown[row]].name.view(); }
    void set_not_responding(Vector<i32> pids)
    {
        m_not_responding = move(pids);
    }
    [[nodiscard]] Optional<size_t> row_of_pid(int pid) const
    {
        for (size_t i = 0; i < m_shown.size(); ++i) {
            if (m_rows[m_shown[i]].pid == pid)
                return i;
        }
        return {};
    }

    // Reads them all again. True if it could.
    bool sample()
    {
        size_t length = 0;
        if (sysctlbyname("kern.proc.all", nullptr, &length, nullptr, 0) != 0)
            return false;
        // Room for any that start meanwhile.
        length += 32 * sizeof(vulture_process_info);
        Vector<vulture_process_info> records;
        MUST(records.try_resize(length / sizeof(vulture_process_info)));
        if (sysctlbyname("kern.proc.all", records.data(), &length, nullptr, 0) != 0)
            return false;
        size_t count = length / sizeof(vulture_process_info);

        double now = now_seconds();
        double elapsed = m_last_sample > 0 ? now - m_last_sample : 0;
        m_last_sample = now;

        Vector<Row> rows;
        double total = 0;
        m_total_threads = 0;
        for (size_t i = 0; i < count; ++i) {
            auto const& record = records[i];
            Row row;
            row.pid = record.pid;
            size_t name_length = strnlen(record.name, sizeof(record.name));
            row.name = MUST(String::from_view(StringView(record.name, name_length)));
            row.cpu_nanoseconds = record.cpu_nanoseconds;
            row.threads = record.threads;
            row.memory = record.resident_bytes;
            row.state = record.state;
            row.user = user_name(record.uid);
            if (elapsed > 0) {
                auto const* previous = m_previous_times.get(record.pid);
                u64 before = previous ? *previous : record.cpu_nanoseconds;
                u64 gained = record.cpu_nanoseconds > before ? record.cpu_nanoseconds - before : 0;
                row.cpu = static_cast<double>(gained) / 1e9 / elapsed * 100;
                total += row.cpu;
            }
            m_total_threads += record.threads;
            rows.append(move(row));
        }
        m_previous_times.clear();
        for (size_t i = 0; i < count; ++i)
            (void)m_previous_times.try_set(records[i].pid, records[i].cpu_nanoseconds);
        m_process_count = count;
        m_total_cpu = total;
        m_rows = move(rows);
        apply_sort();
        return true;
    }

    [[nodiscard]] double total_cpu() const { return m_total_cpu; }
    [[nodiscard]] size_t process_count() const { return m_process_count; }
    [[nodiscard]] u64 total_threads() const { return m_total_threads; }

private:
    [[nodiscard]] String user_name(u32 uid)
    {
        if (auto const* known = m_users.get(uid))
            return MUST(String::from_view(known->view()));
        struct passwd const* account = getpwuid(uid);
        String name;
        if (account) {
            name = MUST(String::from_view(StringView(account->pw_name, strlen(account->pw_name))));
        } else {
            StringBuilder builder;
            builder.appendff("{}", uid);
            name = MUST(builder.to_string());
        }
        (void)m_users.try_set(uid, MUST(String::from_view(name.view())));
        return name;
    }

    [[nodiscard]] int compare(Row const& a, Row const& b) const
    {
        auto by_number = [](auto left, auto right) { return left < right ? -1 : (left > right ? 1 : 0); };
        switch (m_sort_column) {
        case ColumnName:
            return a.name.view().compare(b.name.view());
        case ColumnCPU:
            return by_number(a.cpu, b.cpu);
        case ColumnCPUTime:
            return by_number(a.cpu_nanoseconds, b.cpu_nanoseconds);
        case ColumnThreads:
            return by_number(a.threads, b.threads);
        case ColumnMemory:
            return by_number(a.memory, b.memory);
        case ColumnPID:
            return by_number(a.pid, b.pid);
        case ColumnUser:
            return a.user.view().compare(b.user.view());
        default:
            return 0;
        }
    }

    void apply_sort()
    {
        // In place, by insertion: there are few, and they stay stable between samples.
        for (size_t i = 1; i < m_rows.size(); ++i) {
            Row moving = move(m_rows[i]);
            size_t j = i;
            while (j > 0) {
                int order = compare(m_rows[j - 1], moving);
                if (!m_sort_ascending)
                    order = -order;
                // Ties by process number, so that nothing swaps places for no reason.
                if (order < 0 || (order == 0 && m_rows[j - 1].pid < moving.pid))
                    break;
                m_rows[j] = move(m_rows[j - 1]);
                --j;
            }
            m_rows[j] = move(moving);
        }
        apply_filter();
    }

    void apply_filter()
    {
        m_shown.clear();
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (m_filter.is_empty() || contains_ignoring_case(m_rows[i].name.view(), m_filter.view()))
                m_shown.append(i);
        }
    }

    Vector<i32> m_not_responding;
    Vector<Row> m_rows;
    Vector<size_t> m_shown;
    HashMap<int, u64> m_previous_times;
    HashMap<u32, String> m_users;
    String m_filter;
    size_t m_sort_column { ColumnCPU };
    bool m_sort_ascending { false };
    double m_last_sample { 0 };
    double m_total_cpu { 0 };
    size_t m_process_count { 0 };
    u64 m_total_threads { 0 };
};

// How busy the processors have been, most recent on the right.
class GraphView final : public View {
public:
    void add_sample(float fraction)
    {
        size_t limit = static_cast<size_t>(tokens().metric("monitor.graphSamples"_sv));
        if (m_samples.size() >= limit)
            m_samples.remove(0);
        m_samples.append(clamp(fraction, 0.f, 1.f));
        set_needs_display();
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        graphics.fill({ 0, 0, width(), height() }, t.color("monitor.statsBackground"_sv));
        auto grid = t.color("monitor.graphGrid"_sv);
        for (int i = 1; i < 4; ++i) {
            float y = graphics.snap(height() * static_cast<float>(i) / 4);
            graphics.fill({ 0, y, width(), graphics.pixel() }, grid);
        }
        size_t limit = static_cast<size_t>(t.metric("monitor.graphSamples"_sv));
        if (!m_samples.is_empty()) {
            float step = width() / static_cast<float>(limit - 1);
            float start = width() - static_cast<float>(m_samples.size() - 1) * step;
            Path area;
            Path line;
            auto at = [&](size_t index) {
                return FloatPoint { start + static_cast<float>(index) * step, height() - m_samples[index] * height() };
            };
            area.move_to({ at(0).x, height() });
            for (size_t i = 0; i < m_samples.size(); ++i) {
                area.line_to(at(i));
                if (i == 0)
                    line.move_to(at(i));
                else
                    line.line_to(at(i));
            }
            area.line_to({ at(m_samples.size() - 1).x, height() });
            area.close();
            graphics.painter().fill_path(area, t.color("monitor.graphFill"_sv));
            StrokeStyle stroke;
            stroke.width = 1;
            graphics.painter().stroke_path(line, stroke, t.color("monitor.graph"_sv));
        }
        graphics.stroke_rounded({ 0, 0, width(), height() }, 0, 1, grid);
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Image; }

private:
    Vector<float> m_samples;
};

class Monitor {
public:
    Monitor()
    {
        auto const& t = tokens();
        int width = static_cast<int>(t.metric("monitor.width"_sv));
        int height = static_cast<int>(t.metric("monitor.height"_sv));
        auto& window = Application::the().make_window<UI::Window>("Monitor"_sv, IntRect { 0, 0, width, height }, WS::WindowTitled | WS::WindowResizable | WS::WindowUnifiedTitle);
        m_window = &window;
        window.center_on_screen();
        window.set_sheet_top(t.metric("window.toolbarUnified"_sv));
        auto& stack = window.set_content<StackView>(Axis::Vertical);
        auto& toolbar = stack.add<Toolbar>();
        toolbar.set_title("Monitor"_sv);
        m_stop = &toolbar.add_button(Symbol::ClearCircle, [this] { quit_selected(); }, "Quit Process"_sv);
        m_stop->set_enabled(false);
        toolbar.add_flexible_space();
        auto& search = toolbar.add_search(180);
        search.on_change = [this, &search] {
            m_model.set_filter(search.text());
            reload();
        };

        auto& table = stack.add<TableView>(&m_model);
        m_table = &table;
        table.set_stretch(1, 1);
        table.set_alternating_rows(true);
        table.add_column("Process Name"_sv, 230);
        table.add_column("% CPU"_sv, 70);
        table.add_column("CPU Time"_sv, 90);
        table.add_column("Threads"_sv, 70);
        table.add_column("Memory"_sv, 90);
        table.add_column("PID"_sv, 70);
        table.add_column("User"_sv, 110);
        table.on_selection_change = [this] {
            selection_changed();
        };
        table.header().is_right_aligned = [this](size_t column) { return m_model.is_right_aligned(column); };
        table.header().on_sort = [this](size_t column, bool ascending) {
            m_model.sort_by(column, ascending);
            reload();
        };

        auto& separator = stack.add<Separator>();
        (void)separator;
        float padding = t.metric("monitor.statsPadding"_sv);
        auto& stats = stack.add<StackView>(Axis::Horizontal, padding, Alignment::Fill, Insets::all(padding));
        stats.set_fixed_height(t.metric("monitor.statsHeight"_sv));
        auto add_column = [&stats](float stretch) -> StackView& {
            auto& column = stats.add<StackView>(Axis::Vertical, 4);
            column.set_stretch(stretch, 1);
            return column;
        };
        auto& left = add_column(1);
        m_cpu_label = &left.add<Label>("CPU: \xE2\x80\x94"_sv);
        m_threads_label = &left.add<Label>(StringView {});
        m_processes_label = &left.add<Label>(StringView {});
        m_threads_label->set_style(LabelStyle::Secondary);
        m_processes_label->set_style(LabelStyle::Secondary);
        m_graph = &stats.add<GraphView>();
        m_graph->set_fixed_width(t.metric("monitor.graphWidth"_sv));
        auto& right = add_column(1);
        m_memory_label = &right.add<Label>("Memory: \xE2\x80\x94"_sv);
        m_free_label = &right.add<Label>(StringView {});
        m_free_label->set_style(LabelStyle::Secondary);
        m_compressed_label = &right.add<Label>(StringView {});
        m_compressed_label->set_style(LabelStyle::Secondary);

        window.show();
        (void)window.make_first_responder(&table);
        refresh();
        // What the window server says of those it's been waiting for.
        Application::the().connection().on_shell_state = [this](StringView json) {
            auto state = Desktop::parse_shell_state(json);
            if (state.is_error())
                return;
            Vector<i32> pids;
            for (auto const& application : state.value().applications) {
                if (application.not_responding)
                    pids.append(application.pid);
            }
            m_model.set_not_responding(move(pids));
            refresh();
        };
        (void)Application::the().connection().server().subscribe_shell();
        m_timer = Application::the().add_timer(static_cast<u64>(t.metric("monitor.refreshMilliseconds"_sv)), true, [this] { refresh(); });
    }

private:
    void refresh()
    {
        if (!m_model.sample())
            return;
        reload();
        int processors = max(system_value<int>("hw.ncpu", 1), 1);
        double busy = m_model.total_cpu() / processors;
        m_graph->add_sample(static_cast<float>(busy / 100));
        StringBuilder cpu;
        cpu.append("CPU: "_sv);
        cpu.append(format_percent(busy).view());
        cpu.appendff("% of {} processors", processors);
        m_cpu_label->set_text(cpu.view());
        StringBuilder threads;
        threads.appendff("Threads: {}", m_model.total_threads());
        m_threads_label->set_text(threads.view());
        StringBuilder processes;
        processes.appendff("Processes: {}", m_model.process_count());
        m_processes_label->set_text(processes.view());
        u64 total = system_value<u64>("hw.memsize");
        u64 free = system_value<u64>("hw.freemem");
        StringBuilder memory;
        memory.append("Memory used: "_sv);
        memory.append(format_bytes(total > free ? total - free : 0).view());
        memory.append(" of "_sv);
        memory.append(format_bytes(total).view());
        m_memory_label->set_text(memory.view());
        StringBuilder available;
        available.append("Free: "_sv);
        available.append(format_bytes(free).view());
        m_free_label->set_text(available.view());
        StringBuilder compressed;
        compressed.append("Compressed: "_sv);
        compressed.append(format_bytes(system_value<u64>("vm.compressor.pool_bytes")).view());
        m_compressed_label->set_text(compressed.view());
    }

    void reload()
    {
        m_table->reload();
        // The same process stays selected, wherever it has gone in the list.
        if (m_selected_pid >= 0) {
            m_restoring = true;
            if (auto row = m_model.row_of_pid(m_selected_pid); row.has_value())
                m_table->select_row(row.value());
            else
                m_table->clear_selection();
            m_restoring = false;
        }
    }

    void selection_changed()
    {
        if (m_restoring) {
            m_stop->set_enabled(m_table->selected_row() >= 0);
            return;
        }
        int row = m_table->selected_row();
        m_selected_pid = row >= 0 ? m_model.pid_of(static_cast<size_t>(row)) : -1;
        m_stop->set_enabled(row >= 0);
    }

    void quit_selected()
    {
        int row = m_table->selected_row();
        if (row < 0)
            return;
        int pid = m_model.pid_of(static_cast<size_t>(row));
        StringBuilder message;
        message.append("Are you sure you want to quit \xE2\x80\x9C"_sv);
        message.append(m_model.name_of(static_cast<size_t>(row)));
        message.append("\xE2\x80\x9D?"_sv);
        auto text = MUST(message.to_string());
        Alert alert(text.view(), "Force Quit ends it at once, and anything it had not saved is lost."_sv);
        alert.add_button("Quit"_sv, true);
        alert.add_button("Cancel"_sv);
        alert.add_button("Force Quit"_sv);
        alert.show(*m_window, [this, pid](size_t answer) {
            if (answer == 1)
                return;
            int sent = kill(pid, answer == 2 ? SIGKILL : SIGTERM);
            if (sent != 0) {
                Alert refused("The process could not be quit."_sv, "It belongs to another user."_sv);
                refused.add_button("OK"_sv);
                refused.show(*m_window, [](size_t) { });
            }
            refresh();
        });
    }

    UI::Window* m_window { nullptr };
    TableView* m_table { nullptr };
    ToolbarButton* m_stop { nullptr };
    GraphView* m_graph { nullptr };
    Label* m_cpu_label { nullptr };
    Label* m_threads_label { nullptr };
    Label* m_processes_label { nullptr };
    Label* m_memory_label { nullptr };
    Label* m_free_label { nullptr };
    Label* m_compressed_label { nullptr };
    ProcessModel m_model;
    int m_selected_pid { -1 };
    bool m_restoring { false };
    int m_timer { -1 };
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto application_or_error = Application::create("Monitor"_sv, "com.vulture.Monitor"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Monitor: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    application->set_main_menu(application->standard_menus());
    Monitor monitor;
    printf("monitor: ready\n");
    fflush(stdout);
    return application->exec();
}
