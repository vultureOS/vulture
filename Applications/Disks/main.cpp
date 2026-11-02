/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <Kernel/API/POSIX/sysctl.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

// What is mounted, and how full it is: "vfs.mounts", from the kernel.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

String format_size(u64 bytes)
{
    StringBuilder builder;
    // In thousands, as disks are sold and macOS counts.
    auto with_unit = [&](u64 unit, StringView name) {
        u64 scaled = (bytes * 100 + unit / 2) / unit;
        builder.appendff("{}.{:02} ", scaled / 100, scaled % 100);
        builder.append(name);
    };
    if (bytes >= 1'000'000'000ull)
        with_unit(1'000'000'000ull, "GB"_sv);
    else if (bytes >= 1'000'000ull)
        with_unit(1'000'000ull, "MB"_sv);
    else if (bytes >= 1'000ull)
        with_unit(1'000ull, "KB"_sv);
    else
        builder.appendff("{} bytes", bytes);
    return MUST(builder.to_string());
}

struct Volume {
    String name;
    String type;
    String point;
    String device;
    u64 capacity { 0 };
    u64 free_bytes { 0 };
    bool read_only { false };

    // Those that stand for no storage have nothing to measure.
    [[nodiscard]] bool has_storage() const { return capacity > 0; }
};

Vector<Volume> read_volumes()
{
    Vector<Volume> volumes;
    size_t length = 0;
    if (sysctlbyname("vfs.mounts", nullptr, &length, nullptr, 0) != 0)
        return volumes;
    length += 8 * sizeof(vulture_mount_info);
    Vector<vulture_mount_info> records;
    MUST(records.try_resize(length / sizeof(vulture_mount_info)));
    if (sysctlbyname("vfs.mounts", records.data(), &length, nullptr, 0) != 0)
        return volumes;
    for (size_t i = 0; i < length / sizeof(vulture_mount_info); ++i) {
        auto const& record = records[i];
        Volume volume;
        StringView point(record.point, strnlen(record.point, sizeof(record.point)));
        volume.point = MUST(String::from_view(point));
        volume.type = MUST(String::from_view(StringView(record.type, strnlen(record.type, sizeof(record.type)))));
        volume.device = MUST(String::from_view(StringView(record.device, strnlen(record.device, sizeof(record.device)))));
        volume.capacity = record.blocks * record.block_size;
        volume.free_bytes = record.free_blocks * record.block_size;
        volume.read_only = record.read_only != 0;
        if (point == "/"_sv) {
            volume.name = MUST(String::from_view("Vulture HD"_sv));
        } else {
            size_t start = point.length();
            while (start > 0 && point[start - 1] != '/')
                --start;
            volume.name = MUST(String::from_view(point.substring_view(start, point.length() - start)));
        }
        volumes.append(move(volume));
    }
    return volumes;
}

// Used on the left, free on the right.
class CapacityBar final : public View {
public:
    void set_fraction(float fraction)
    {
        m_fraction = clamp(fraction, 0.f, 1.f);
        set_needs_display();
    }

    [[nodiscard]] FloatSize intrinsic_size() const override { return { -1, tokens().metric("disks.barHeight"_sv) }; }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        float radius = t.metric("disks.barRadius"_sv);
        FloatRect whole { 0, 0, width(), height() };
        graphics.fill_rounded(whole, radius, t.color("disks.free"_sv));
        if (m_fraction <= 0)
            return;
        graphics.save();
        graphics.clip({ 0, 0, width() * m_fraction, height() });
        graphics.fill_rounded(whole, radius, t.color("disks.used"_sv));
        graphics.restore();
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::ProgressIndicator; }

private:
    float m_fraction { 0 };
};

class DiskContent final : public View {
public:
    void draw(Graphics& graphics) override
    {
        auto sidebar = tokens().metric("disks.sidebarWidth"_sv);
        graphics.fill({ sidebar, 0, width() - sidebar, height() }, tokens().color("windowBackground"_sv));
    }

    void layout() override
    {
        for (auto const& child : children())
            child->set_frame({ 0, 0, width(), height() });
    }
};

class Disks {
public:
    Disks()
    {
        auto const& t = tokens();
        int width = static_cast<int>(t.metric("disks.width"_sv));
        int height = static_cast<int>(t.metric("disks.height"_sv));
        auto& window = Application::the().make_window<UI::Window>("Disks"_sv, IntRect { 0, 0, width, height }, WS::WindowTitled | WS::WindowResizable | WS::WindowUnifiedTitle);
        window.center_on_screen();
        window.set_transparent(true);
        window.set_sidebar_material(t.metric("disks.sidebarWidth"_sv));
        window.set_sheet_top(t.metric("window.toolbarUnified"_sv));
        auto& root = window.set_content<DiskContent>();
        auto& split = root.add<SplitView>();
        float sidebar_width = t.metric("disks.sidebarWidth"_sv);
        m_sidebar = &split.add_pane<Sidebar>(sidebar_width, sidebar_width);
        m_sidebar->set_top_padding(t.metric("window.toolbarCompact"_sv));
        m_sidebar->on_select = [this](size_t row) { show_row(row); };

        auto& right = split.add_pane<StackView>(0, 300, Axis::Vertical);
        auto& toolbar = right.add<Toolbar>();
        toolbar.set_left_padding(t.metric("toolbar.paddingRight"_sv));
        m_title = &toolbar.add_item<Label>("Disks"_sv, t.named_font("headline"_sv));
        toolbar.add_flexible_space();
        (void)toolbar.add_button(Symbol::Refresh, [this] { reload(); }, "Refresh"_sv);

        float padding = t.metric("disks.padding"_sv);
        auto& details = right.add<StackView>(Axis::Vertical, 10, Alignment::Fill, Insets::all(padding));
        details.set_stretch(1, 1);
        auto& heading = details.add<StackView>(Axis::Horizontal, 12, Alignment::Center);
        heading.set_fixed_height(44);
        m_heading = &heading.add<Label>(StringView {}, t.named_font("title2"_sv));
        m_heading->set_stretch(1, 0);
        m_summary = &details.add<Label>(StringView {});
        m_summary->set_style(LabelStyle::Secondary);
        m_bar = &details.add<CapacityBar>();
        m_usage = &details.add<Label>(StringView {});
        (void)details.add<Separator>();
        char const* names[] = { "Mount Point", "Type", "Device", "Capacity", "Available", "Used" };
        for (size_t i = 0; i < array_size(names); ++i) {
            auto& row = details.add<StackView>(Axis::Horizontal, 8, Alignment::Center);
            auto& name = row.add<Label>(StringView(names[i], strlen(names[i])));
            name.set_style(LabelStyle::Secondary);
            name.set_fixed_width(110);
            m_values[i] = &row.add<Label>(StringView {});
            m_values[i]->set_stretch(1, 0);
        }
        auto& filler = details.add<View>();
        filler.set_stretch(0, 1);

        window.show();
        reload();
    }

private:
    void reload()
    {
        m_volumes = read_volumes();
        m_sidebar_rows.clear();
        // (A sidebar's rows are for good, once it has them; volumes do not come and go
        // while nothing here unmounts them.)
        if (!m_filled) {
            m_filled = true;
            m_sidebar->add_section("Internal"_sv);
            for (size_t i = 0; i < m_volumes.size(); ++i) {
                if (m_volumes[i].has_storage())
                    (void)m_sidebar->add_row(m_volumes[i].name.view(), Symbol::Drive);
            }
            m_sidebar->add_section("System"_sv);
            for (size_t i = 0; i < m_volumes.size(); ++i) {
                if (!m_volumes[i].has_storage())
                    (void)m_sidebar->add_row(m_volumes[i].name.is_empty() ? m_volumes[i].type.view() : m_volumes[i].name.view(), Symbol::File);
            }
        }
        for (size_t i = 0; i < m_volumes.size(); ++i) {
            if (m_volumes[i].has_storage())
                m_sidebar_rows.append(i);
        }
        for (size_t i = 0; i < m_volumes.size(); ++i) {
            if (!m_volumes[i].has_storage())
                m_sidebar_rows.append(i);
        }
        int selected = m_sidebar->selected_row();
        if (selected < 0 && !m_sidebar_rows.is_empty())
            m_sidebar->select_row(0);
        else if (selected >= 0)
            show_row(static_cast<size_t>(selected));
    }

    void show_row(size_t row)
    {
        if (row >= m_sidebar_rows.size())
            return;
        auto const& volume = m_volumes[m_sidebar_rows[row]];
        m_title->set_text(volume.name.view());
        m_heading->set_text(volume.name.view());
        StringBuilder summary;
        summary.append(volume.type.view());
        if (volume.read_only)
            summary.append(" \xE2\x80\x94 Read Only"_sv);
        m_summary->set_text(summary.view());
        u64 used = volume.capacity > volume.free_bytes ? volume.capacity - volume.free_bytes : 0;
        m_bar->set_hidden(!volume.has_storage());
        m_usage->set_hidden(!volume.has_storage());
        if (volume.has_storage()) {
            m_bar->set_fraction(static_cast<float>(static_cast<double>(used) / static_cast<double>(volume.capacity)));
            StringBuilder usage;
            usage.append(format_size(used).view());
            usage.append(" used of "_sv);
            usage.append(format_size(volume.capacity).view());
            m_usage->set_text(usage.view());
        }
        auto set = [&](size_t index, StringView text) { m_values[index]->set_text(text.is_empty() ? "\xE2\x80\x94"_sv : text); };
        set(0, volume.point.view());
        set(1, volume.type.view());
        set(2, volume.device.view());
        auto capacity = format_size(volume.capacity);
        auto available = format_size(volume.free_bytes);
        auto used_text = format_size(used);
        set(3, volume.has_storage() ? capacity.view() : StringView {});
        set(4, volume.has_storage() ? available.view() : StringView {});
        set(5, volume.has_storage() ? used_text.view() : StringView {});
    }

    Sidebar* m_sidebar { nullptr };
    Label* m_title { nullptr };
    Label* m_heading { nullptr };
    Label* m_summary { nullptr };
    Label* m_usage { nullptr };
    CapacityBar* m_bar { nullptr };
    Label* m_values[6] {};
    Vector<Volume> m_volumes;
    Vector<size_t> m_sidebar_rows;
    bool m_filled { false };
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    auto application_or_error = Application::create("Disks"_sv, "com.vulture.Disks"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Disks: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    application->set_main_menu(application->standard_menus());
    Disks disks;
    printf("disks: ready\n");
    fflush(stdout);
    return application->exec();
}
