/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <Kernel/API/POSIX/sysctl.h>
#include <LibDiskManagement/Disks.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/Menu.h>
#include <LibUI/Panels.h>
#include <LibUI/TextField.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

// What's plugged in, what's on it, how full that is, and what can be done about it: First Aid, Erase, Mount, Eject. What
// changes anything goes through diskarbitrationd.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

struct Volume {
    DiskManagement::Disk disk;
    String name;
    u64 capacity { 0 };
    u64 free_bytes { 0 };
    bool has_usage { false };
    bool read_only { false };

    [[nodiscard]] bool can_be_mounted() const { return !disk.content().is_empty() && disk.content() != "vulture-system"_sv && disk.content() != "vulture-data"_sv; }
    [[nodiscard]] bool is_mounted() const { return !disk.mount_point.is_empty(); }
    [[nodiscard]] bool is_system() const { return disk.content() == "vulture-system"_sv || disk.content() == "vulture-data"_sv; }
    [[nodiscard]] bool can_be_erased() const { return !is_system() && disk.info.removable; }
};

Vector<vulture_mount_info> read_mounts()
{
    Vector<vulture_mount_info> records;
    size_t length = 0;
    if (sysctlbyname("vfs.mounts", nullptr, &length, nullptr, 0) != 0)
        return records;
    length += 8 * sizeof(vulture_mount_info);
    MUST(records.try_resize(length / sizeof(vulture_mount_info)));
    if (sysctlbyname("vfs.mounts", records.data(), &length, nullptr, 0) != 0)
        length = 0;
    MUST(records.try_resize(length / sizeof(vulture_mount_info)));
    return records;
}

// Each that has something on it that's worth saying, with what it knows about how full it is.
Vector<Volume> read_volumes()
{
    Vector<Volume> volumes;
    auto disks = DiskManagement::list_disks();
    if (disks.is_error())
        return volumes;
    auto const mounts = read_mounts();
    for (auto& disk : disks.value()) {
        // A disk with partitions is only what they're on.
        bool has_partitions = false;
        for (auto const& other : disks.value())
            has_partitions = has_partitions || (!other.is_whole() && other.whole() == disk.name());
        if (disk.is_whole() && has_partitions)
            continue;
        if (disk.content().is_empty() && !disk.is_whole())
            continue;
        // What the firmware starts from is none of anyone's business here, as on a Mac.
        if (disk.mount_point.view() == "/Volumes/EFI"_sv)
            continue;
        Volume volume;
        StringView const label = disk.label().is_empty() ? "Untitled"_sv : disk.label();
        volume.name = MUST(String::from_view(label));
        volume.capacity = disk.size();
        volume.read_only = disk.info.read_only;
        for (auto const& mount : mounts) {
            if (strcmp(mount.device, disk.info.name) != 0)
                continue;
            volume.capacity = mount.blocks * mount.block_size;
            volume.free_bytes = mount.free_blocks * mount.block_size;
            volume.read_only = mount.read_only;
            volume.has_usage = true;
        }
        volume.disk = move(disk);
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

static constexpr float sheet_width = 440;

// As Disk Utility calls them.
[[nodiscard]] static StringView format_title(StringView content)
{
    if (content == "exfat"_sv)
        return "ExFAT"_sv;
    if (content == "msdos"_sv)
        return "MS-DOS (FAT)"_sv;
    if (content == "cd9660"_sv)
        return "ISO 9660"_sv;
    if (content == "vulture-system"_sv)
        return "Vulture (System)"_sv;
    if (content == "vulture-data"_sv)
        return "Vulture (Data)"_sv;
    return content;
}

// What a sheet warns of is read in full, on as many lines as that takes.
static void wrap_to_sheet(Label& label)
{
    float const width = sheet_width - 40;
    label.set_wraps(true);
    label.set_fixed_width(width);
    label.set_frame({ 0, 0, width, 16 });
    label.set_fixed_height(label.intrinsic_size().height);
}

class EraseSheet final : public StackView {
public:
    EraseSheet(StringView volume_name, Function<void(StringView format, StringView name)> erase, Function<void()> cancel)
        : StackView(Axis::Vertical, 12, Alignment::Fill, Insets::all(20))
    {
        auto const& t = tokens();
        StringBuilder title;
        title.appendff("Erase \xE2\x80\x9C{}\xE2\x80\x9D?", volume_name);
        add<Label>(title.view(), t.named_font("headline"_sv));
        auto& warning = add<Label>("Erasing this will delete everything on it, and it can't be undone. Give it a name and choose a format."_sv);
        warning.set_style(LabelStyle::Secondary);
        wrap_to_sheet(warning);
        auto& name_row = add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& name_label = name_row.add<Label>("Name:"_sv);
        name_label.set_fixed_width(60);
        m_name = &name_row.add<TextField>();
        m_name->set_text(volume_name);
        m_name->set_stretch(1, 0);
        auto& format_row = add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& format_label = format_row.add<Label>("Format:"_sv);
        format_label.set_fixed_width(60);
        m_format = &format_row.add<PopUpButton>();
        m_format->set_accessibility_label("Format"_sv);
        m_format->add_item("ExFAT"_sv);
        m_format->add_item("MS-DOS (FAT)"_sv);
        auto& buttons = add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& filler = buttons.add<View>();
        filler.set_stretch(1, 0);
        auto& no = buttons.add<Button>("Cancel"_sv);
        no.on_action = move(cancel);
        auto& yes = buttons.add<Button>("Erase"_sv, ButtonStyle::Default);
        yes.on_action = [this, erase = move(erase)] {
            erase(m_format->selected_index() == 0 ? "exfat"_sv : "msdos"_sv, m_name->text());
        };
    }

private:
    TextField* m_name { nullptr };
    PopUpButton* m_format { nullptr };
};

class PartitionSheet final : public StackView {
public:
    PartitionSheet(StringView volume_name, Function<void(int count, StringView format)> partition, Function<void()> cancel)
        : StackView(Axis::Vertical, 12, Alignment::Fill, Insets::all(20))
    {
        auto const& t = tokens();
        StringBuilder title;
        title.appendff("Partition \xE2\x80\x9C{}\xE2\x80\x9D?", volume_name);
        add<Label>(title.view(), t.named_font("headline"_sv));
        auto& warning = add<Label>("Partitioning will delete everything on this disk, and it can't be undone. It's divided into equal parts."_sv);
        warning.set_style(LabelStyle::Secondary);
        wrap_to_sheet(warning);
        auto& count_row = add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& count_label = count_row.add<Label>("Parts:"_sv);
        count_label.set_fixed_width(60);
        m_count = &count_row.add<PopUpButton>();
        m_count->set_accessibility_label("Parts"_sv);
        m_count->add_item("Two"_sv);
        m_count->add_item("Three"_sv);
        m_count->add_item("Four"_sv);
        auto& format_row = add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& format_label = format_row.add<Label>("Format:"_sv);
        format_label.set_fixed_width(60);
        m_format = &format_row.add<PopUpButton>();
        m_format->set_accessibility_label("Format"_sv);
        m_format->add_item("ExFAT"_sv);
        m_format->add_item("MS-DOS (FAT)"_sv);
        auto& buttons = add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& filler = buttons.add<View>();
        filler.set_stretch(1, 0);
        auto& no = buttons.add<Button>("Cancel"_sv);
        no.on_action = move(cancel);
        auto& yes = buttons.add<Button>("Partition"_sv, ButtonStyle::Default);
        yes.on_action = [this, partition = move(partition)] {
            partition(m_count->selected_index() + 2, m_format->selected_index() == 0 ? "exfat"_sv : "msdos"_sv);
        };
    }

private:
    PopUpButton* m_count { nullptr };
    PopUpButton* m_format { nullptr };
};

class Disks {
public:
    Disks()
    {
        auto const& t = tokens();
        int width = static_cast<int>(t.metric("disks.width"_sv));
        int height = static_cast<int>(t.metric("disks.height"_sv));
        auto& window = Application::the().make_window<UI::Window>("Disks"_sv, IntRect { 0, 0, width, height }, WS::WindowTitled | WS::WindowResizable | WS::WindowUnifiedTitle);
        m_window = &window;
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
        m_first_aid = &toolbar.add_button(Symbol::FirstAid, [this] { run_first_aid(); }, "First Aid"_sv);
        m_erase = &toolbar.add_button(Symbol::Erase, [this] { ask_to_erase(); }, "Erase\xE2\x80\xA6"_sv);
        m_partition = &toolbar.add_button(Symbol::Partition, [this] { ask_to_partition(); }, "Partition\xE2\x80\xA6"_sv);
        m_mount = &toolbar.add_button(Symbol::Mount, [this] { mount_or_unmount(); }, "Mount"_sv);
        m_eject = &toolbar.add_button(Symbol::Eject, [this] { eject(); }, "Eject"_sv);
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
        char const* names[] = { "Mount Point", "Type", "Device", "Capacity", "Available", "Used", "Connection" };
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
        // What's plugged in, as it's plugged in.
        m_timer = Application::the().add_timer(1000, true, [this] {
            auto const now = read_volumes();
            bool changed = now.size() != m_volumes.size();
            for (size_t i = 0; i < now.size() && !changed; ++i)
                changed = now[i].disk.name() != m_volumes[i].disk.name() || now[i].is_mounted() != m_volumes[i].is_mounted() || now[i].name.view() != m_volumes[i].name.view();
            if (changed)
                reload();
        });
    }

private:
    void reload()
    {
        bool const had_selection = m_sidebar->selected_row() >= 0 && static_cast<size_t>(m_sidebar->selected_row()) < m_rows.size();
        auto const previous = had_selection ? MUST(String::from_view(m_volumes[m_rows[static_cast<size_t>(m_sidebar->selected_row())]].disk.name())) : String {};
        auto const previous_whole = had_selection ? MUST(String::from_view(m_volumes[m_rows[static_cast<size_t>(m_sidebar->selected_row())]].disk.whole())) : String {};
        m_volumes = read_volumes();
        m_rows.clear();
        m_sidebar->clear();
        auto const add_section = [&](StringView title, auto matches) {
            bool any = false;
            for (size_t i = 0; i < m_volumes.size(); ++i) {
                if (!matches(m_volumes[i]))
                    continue;
                if (!any)
                    m_sidebar->add_section(title);
                any = true;
                (void)m_sidebar->add_row(m_volumes[i].name.view(), Symbol::Drive);
                m_rows.append(i);
            }
        };
        add_section("Internal"_sv, [](Volume const& volume) { return !volume.disk.info.removable; });
        add_section("External"_sv, [](Volume const& volume) { return volume.disk.info.removable && strcmp(volume.disk.info.bus, "disk image") != 0; });
        add_section("Disk Images"_sv, [](Volume const& volume) { return strcmp(volume.disk.info.bus, "disk image") == 0; });
        // That one; or, if it's been erased or divided since, the first of what's on that disk.
        size_t chosen = 0;
        bool found = false;
        for (size_t row = 0; row < m_rows.size() && !found; ++row) {
            found = m_volumes[m_rows[row]].disk.name() == previous.view();
            if (found)
                chosen = row;
        }
        for (size_t row = 0; row < m_rows.size() && !found; ++row) {
            found = m_volumes[m_rows[row]].disk.whole() == previous_whole.view();
            if (found)
                chosen = row;
        }
        if (!m_rows.is_empty()) {
            m_sidebar->select_row(chosen);
            show_row(chosen);
        }
    }

    [[nodiscard]] Volume const* selected() const
    {
        int const row = m_sidebar->selected_row();
        if (row < 0 || static_cast<size_t>(row) >= m_rows.size())
            return nullptr;
        return &m_volumes[m_rows[static_cast<size_t>(row)]];
    }

    void show_row(size_t row)
    {
        if (row >= m_rows.size())
            return;
        auto const& volume = m_volumes[m_rows[row]];
        m_title->set_text(volume.name.view());
        m_heading->set_text(volume.name.view());
        StringBuilder summary;
        summary.append(volume.disk.content().is_empty() ? "No file system"_sv : format_title(volume.disk.content()));
        if (volume.read_only)
            summary.append(" \xE2\x80\x94 Read Only"_sv);
        m_summary->set_text(summary.view());
        u64 const used = volume.capacity > volume.free_bytes ? volume.capacity - volume.free_bytes : 0;
        m_bar->set_hidden(!volume.has_usage);
        m_usage->set_hidden(!volume.has_usage);
        if (volume.has_usage && volume.capacity) {
            m_bar->set_fraction(static_cast<float>(static_cast<double>(used) / static_cast<double>(volume.capacity)));
            StringBuilder usage;
            usage.append(DiskManagement::size_text(used).view());
            usage.append(" used of "_sv);
            usage.append(DiskManagement::size_text(volume.capacity).view());
            m_usage->set_text(usage.view());
        }
        auto const set = [&](size_t index, StringView text) { m_values[index]->set_text(text.is_empty() ? "\xE2\x80\x94"_sv : text); };
        auto const capacity = DiskManagement::size_text(volume.capacity);
        auto const available = DiskManagement::size_text(volume.free_bytes);
        auto const used_text = DiskManagement::size_text(used);
        set(0, volume.disk.mount_point.view());
        set(1, format_title(volume.disk.content()));
        set(2, volume.disk.name());
        set(3, capacity.view());
        set(4, volume.has_usage ? available.view() : StringView {});
        set(5, volume.has_usage ? used_text.view() : StringView {});
        set(6, { volume.disk.info.bus, strlen(volume.disk.info.bus) });
        m_first_aid->set_enabled(volume.disk.content() == "msdos"_sv || volume.disk.content() == "exfat"_sv);
        m_erase->set_enabled(volume.can_be_erased());
        m_partition->set_enabled(volume.can_be_erased());
        m_mount->set_enabled(volume.can_be_mounted() && !volume.is_system());
        m_mount->set_accessibility_label(volume.is_mounted() ? "Unmount"_sv : "Mount"_sv);
        m_eject->set_enabled(volume.disk.info.removable);
    }

    void tell(StringView message, StringView information)
    {
        auto* alert = new Alert(message, information);
        alert->add_button("OK"_sv, true);
        alert->show(*m_window, [alert](size_t) { delete alert; });
    }

    void report_failure(StringView what, Error const& error)
    {
        StringBuilder message;
        message.appendff("Couldn't {}.", what);
        tell(message.view(), { strerror(error.code()), strlen(strerror(error.code())) });
    }

    void run_first_aid()
    {
        auto const* volume = selected();
        if (!volume)
            return;
        StringBuilder message;
        message.appendff("Run First Aid on \xE2\x80\x9C{}\xE2\x80\x9D?", volume->name.view());
        auto* alert = new Alert(message.view(), "First Aid checks the volume, and repairs what it can. It's unmounted while that goes on."_sv);
        alert->add_button("Cancel"_sv);
        alert->add_button("Run"_sv, true);
        auto const device = MUST(String::from_view(volume->disk.name()));
        alert->show(*m_window, [this, alert, device](size_t button) {
            delete alert;
            if (button != 1)
                return;
            StringBuilder line;
            line.appendff("repair {}", device.view());
            bool clean = false;
            StringBuilder lines;
            auto const result = DiskManagement::request(line.view(), [&](StringView text) {
                if (text == "clean"_sv) {
                    clean = true;
                } else if (text != "damaged"_sv) {
                    lines.append(text);
                    lines.append('\n');
                }
            });
            if (result.is_error()) {
                report_failure("run First Aid"_sv, result.error());
                return;
            }
            printf("disks: first aid finished: %s\n", clean ? "clean" : "damaged");
            fflush(stdout);
            tell(clean ? "First Aid process has completed successfully."_sv : "First Aid found corruption that needs to be repaired."_sv, lines.view());
            reload();
        });
    }

    void ask_to_erase()
    {
        auto const* volume = selected();
        if (!volume)
            return;
        auto const device = MUST(String::from_view(volume->disk.name()));
        auto const whole = volume->disk.is_whole();
        auto* sheet = new EraseSheet(
            volume->name.view(),
            [this, device, whole](StringView format, StringView name) {
                StringBuilder line;
                line.appendff("{} {} {} {}", whole ? "erase-disk"_sv : "erase-volume"_sv, device.view(), format, name);
                auto const line_text = MUST(line.to_string());
                Sheet::dismiss(*m_window);
                auto const result = DiskManagement::request(line_text.view());
                if (result.is_error()) {
                    report_failure("erase it"_sv, result.error());
                    return;
                }
                printf("disks: erased\n");
                fflush(stdout);
                reload();
            },
            [this] { Sheet::dismiss(*m_window); });
        auto const height = sheet->preferred_size().height;
        Sheet::present(*m_window, adopt_own(*static_cast<View*>(sheet)), { sheet_width, height });
    }

    void ask_to_partition()
    {
        auto const* volume = selected();
        if (!volume)
            return;
        auto const whole = MUST(String::from_view(volume->disk.whole()));
        u64 const sectors = volume->disk.info.sectors;
        auto* sheet = new PartitionSheet(
            volume->name.view(),
            [this, whole, sectors](int count, StringView format) {
                StringBuilder line;
                line.appendff("partition {} ", whole.view());
                for (int i = 0; i < count; ++i) {
                    if (i)
                        line.append('\t');
                    // (The last gets what's left, which is what a table's own size takes from it.)
                    line.appendff("{} {} UNTITLED {}", format, i + 1 == count ? 0 : sectors / static_cast<u64>(count), i + 1);
                }
                auto const line_text = MUST(line.to_string());
                Sheet::dismiss(*m_window);
                auto const result = DiskManagement::request(line_text.view());
                if (result.is_error()) {
                    report_failure("partition it"_sv, result.error());
                    return;
                }
                printf("disks: partitioned\n");
                fflush(stdout);
                reload();
            },
            [this] { Sheet::dismiss(*m_window); });
        auto const height = sheet->preferred_size().height;
        Sheet::present(*m_window, adopt_own(*static_cast<View*>(sheet)), { sheet_width, height });
    }

    void mount_or_unmount()
    {
        auto const* volume = selected();
        if (!volume)
            return;
        StringBuilder line;
        line.appendff("{} {}", volume->is_mounted() ? "unmount"_sv : "mount"_sv, volume->disk.name());
        auto const was_mounted = volume->is_mounted();
        auto const result = DiskManagement::request(line.view());
        if (result.is_error())
            report_failure(was_mounted ? "unmount it"_sv : "mount it"_sv, result.error());
        reload();
    }

    void eject()
    {
        auto const* volume = selected();
        if (!volume)
            return;
        StringBuilder line;
        line.appendff("eject {}", volume->disk.name());
        auto const result = DiskManagement::request(line.view());
        if (result.is_error())
            report_failure("eject it"_sv, result.error());
        reload();
    }

    UI::Window* m_window { nullptr };
    Sidebar* m_sidebar { nullptr };
    Label* m_title { nullptr };
    Label* m_heading { nullptr };
    Label* m_summary { nullptr };
    Label* m_usage { nullptr };
    CapacityBar* m_bar { nullptr };
    Label* m_values[7] {};
    ToolbarButton* m_first_aid { nullptr };
    ToolbarButton* m_erase { nullptr };
    ToolbarButton* m_partition { nullptr };
    ToolbarButton* m_mount { nullptr };
    ToolbarButton* m_eject { nullptr };
    Vector<Volume> m_volumes;
    Vector<size_t> m_rows;
    int m_timer { -1 };
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
