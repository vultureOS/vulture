/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Application.h>
#include <LibUI/Builder.h>
#include <LibUI/Containers.h>
#include <LibUI/Layout.h>
#include <LibUI/Motion.h>
#include <LibUI/Panels.h>
#include <LibUI/ScrollView.h>
#include <LibUI/TableView.h>
#include <LibUI/TextView.h>
#include <stdio.h>
#include <string.h>

// All of pranaUI on show. Keys: 1 to 6 for the pages, d for dark and light, m for a
// menu, s for an alert, p for a popover, o for an open panel, c and f for the color
// and font panels. With --still nothing moves or blinks, for golden screenshots.

using namespace UI;
using namespace UI::Build;
using namespace Gfx;

namespace {

constexpr ControlSize s_sizes[4] = { ControlSize::Large, ControlSize::Regular, ControlSize::Small, ControlSize::Mini };

void add_row_title(View& parent, StringView title)
{
    auto& label = parent.add<Label>(title, Appearance::the().named_font("headline"_sv));
    (void)label;
}

StackView& add_row(View& parent, float spacing = 12)
{
    auto& row = parent.add<StackView>(Axis::Horizontal, spacing, Alignment::Center);
    return row;
}

class Page final : public StackView {
public:
    Page()
        : StackView(Axis::Vertical, 12, Alignment::Fill, Insets { 20, 24, 20, 24 })
    {
    }
    void draw(Graphics& graphics) override
    {
        graphics.fill({ 0, 0, width(), height() }, Appearance::the().color("windowBackground"_sv));
    }
};

void populate_buttons(Page& page)
{
    add_row_title(page, "Push buttons"_sv);
    {
        auto& row = add_row(page);
        for (auto size : s_sizes) {
            auto& button = row.add<Button>("Button"_sv);
            button.set_control_size(size);
        }
        auto& default_one = row.add<Button>("Default"_sv, ButtonStyle::Default);
        (void)default_one;
        auto& disabled = row.add<Button>("Disabled"_sv);
        disabled.set_enabled(false);
        (void)row.add<Button>("Textured"_sv, ButtonStyle::Textured);
        (void)row.add<Button>(StringView {}, ButtonStyle::Help);
        auto& circular = row.add<Button>(StringView {}, ButtonStyle::Circular);
        circular.set_symbol(Symbol::Plus);
        (void)row.add<Button>(StringView {}, ButtonStyle::RoundedDisclosure);
        (void)row.add<Button>(StringView {}, ButtonStyle::Disclosure);
    }
    add_row_title(page, "Checkboxes"_sv);
    {
        auto& row = add_row(page, 16);
        for (auto size : s_sizes) {
            auto& box = row.add<Checkbox>("Check"_sv, ToggleState::On);
            box.set_control_size(size);
        }
        (void)row.add<Checkbox>("Off"_sv);
        (void)row.add<Checkbox>("Mixed"_sv, ToggleState::Mixed);
        auto& disabled = row.add<Checkbox>("Disabled"_sv, ToggleState::On);
        disabled.set_enabled(false);
    }
    add_row_title(page, "Radio buttons"_sv);
    {
        auto& row = add_row(page, 16);
        (void)row.add<RadioButton>("First"_sv, true);
        (void)row.add<RadioButton>("Second"_sv);
        (void)row.add<RadioButton>("Third"_sv);
        auto& small = row.add<RadioButton>("Small"_sv, true, 1);
        small.set_control_size(ControlSize::Small);
        auto& mini = row.add<RadioButton>("Mini"_sv, false, 1);
        mini.set_control_size(ControlSize::Mini);
    }
    add_row_title(page, "Switches"_sv);
    {
        auto& row = add_row(page, 16);
        for (auto size : s_sizes) {
            auto& on = row.add<Switch>(true);
            on.set_control_size(size);
        }
        for (auto size : s_sizes) {
            auto& off = row.add<Switch>(false);
            off.set_control_size(size);
        }
    }
    add_row_title(page, "Segmented controls"_sv);
    {
        auto& row = add_row(page, 16);
        for (auto size : s_sizes) {
            auto& control = row.add<SegmentedControl>();
            control.add_segment("One"_sv);
            control.add_segment("Two"_sv);
            control.add_segment("Three"_sv);
            control.set_selected(1);
            control.set_control_size(size);
        }
    }
    auto& rest = page.add<View>();
    rest.set_stretch(0, 1);
}

void populate_values(Page& page)
{
    add_row_title(page, "Pop-up buttons"_sv);
    {
        auto& row = add_row(page, 16);
        for (auto size : s_sizes) {
            auto& button = row.add<PopUpButton>();
            button.add_item("Popup"_sv);
            button.add_item("Another choice"_sv);
            button.set_control_size(size);
        }
        auto& pull = row.add<PopUpButton>(true);
        pull.set_title("Pull"_sv);
        pull.add_item("Item"_sv);
    }
    add_row_title(page, "Combo box"_sv);
    {
        auto& row = add_row(page, 16);
        auto& combo = row.add<ComboBox>();
        combo.set_text("Combo"_sv);
        combo.add_item("Combo"_sv);
        combo.add_item("Box"_sv);
        combo.set_fixed_width(180);
        auto& stepper = row.add<Stepper>(0, 10, 3);
        (void)stepper;
        auto& well = row.add<ColorWell>(Color { 0, 122, 255, 255 });
        well.on_action = [&well] {
            ColorPanel::the().show(well.color(), [&well](Color color) { well.set_color(color); });
        };
    }
    add_row_title(page, "Sliders"_sv);
    {
        auto& row = add_row(page, 24);
        auto& plain = row.add<Slider>(0.0f, 100.0f, 40.0f);
        plain.set_fixed_width(200);
        auto& ticks = row.add<Slider>(0.0f, 100.0f, 40.0f);
        ticks.set_tick_count(11);
        ticks.set_fixed_width(200);
        auto& disabled = row.add<Slider>(0.0f, 100.0f, 40.0f);
        disabled.set_enabled(false);
        disabled.set_fixed_width(120);
        auto& vertical = row.add<Slider>(0.0f, 100.0f, 40.0f);
        vertical.set_vertical(true);
        vertical.set_fixed_height(80);
    }
    add_row_title(page, "Progress"_sv);
    {
        auto& row = add_row(page, 24);
        auto& bar = row.add<ProgressBar>(0.4f);
        bar.set_fixed_width(200);
        auto& thin = row.add<ProgressBar>(0.7f);
        thin.set_control_size(ControlSize::Small);
        thin.set_fixed_width(200);
        (void)row.add<Spinner>();
        (void)row.add<Spinner>(ControlSize::Small);
    }
    add_row_title(page, "Level"_sv);
    {
        auto& row = add_row(page, 24);
        auto& normal = row.add<LevelIndicator>(0.4f);
        normal.set_fixed_width(180);
        auto& warning = row.add<LevelIndicator>(0.85f);
        warning.set_fixed_width(180);
        auto& critical = row.add<LevelIndicator>(0.95f);
        critical.set_fixed_width(180);
    }
    auto& rest = page.add<View>();
    rest.set_stretch(0, 1);
}

void populate_text(Page& page)
{
    add_row_title(page, "Text fields"_sv);
    {
        auto& row = add_row(page, 12);
        for (auto size : s_sizes) {
            auto& field = row.add<TextField>("Text"_sv);
            field.set_control_size(size);
            field.set_fixed_width(120);
        }
    }
    {
        auto& row = add_row(page, 12);
        auto& rounded = row.add<TextField>("Rounded"_sv, FieldStyle::Rounded);
        rounded.set_fixed_width(140);
        auto& placeholder = row.add<TextField>();
        placeholder.set_placeholder("Placeholder"_sv);
        placeholder.set_fixed_width(140);
        auto& disabled = row.add<TextField>("Disabled"_sv);
        disabled.set_enabled(false);
        disabled.set_fixed_width(140);
        auto& secure = row.add<TextField>("password"_sv);
        secure.set_secure(true);
        secure.set_fixed_width(140);
        auto& search = row.add<SearchField>();
        search.set_fixed_width(160);
    }
    add_row_title(page, "Type"_sv);
    {
        StringView names[] = { "largeTitle"_sv, "title1"_sv, "title2"_sv, "title3"_sv, "headline"_sv, "body"_sv, "callout"_sv, "subheadline"_sv, "footnote"_sv };
        for (auto name : names) {
            auto& label = page.add<Label>(name, Appearance::the().named_font(name));
            (void)label;
        }
    }
    add_row_title(page, "Text view"_sv);
    auto& scroll = page.add<ScrollView>();
    scroll.set_scrolls_horizontally(false);
    scroll.set_stretch(0, 1);
    auto& view = scroll.make_document<TextView>();
    StringBuilder text;
    for (int i = 0; i < 40; ++i) {
        text.append("Everything here is drawn by our own code: the rasterizer, the fonts' shaper, the compositor, and these words wrapping as the window changes size. "_sv);
    }
    view.set_text(text.view());
}

class FilesModel final : public TableModel {
public:
    [[nodiscard]] size_t row_count() const override { return 60; }
    [[nodiscard]] String text_at(size_t row, size_t column) const override
    {
        StringBuilder builder;
        if (column == 0)
            builder.appendff("Document {}.txt", row + 1);
        else if (column == 1)
            builder.appendff("{} KB", (row * 37) % 900 + 4);
        else
            builder.append("Plain Text"_sv);
        return MUST(builder.to_string());
    }
    [[nodiscard]] bool has_symbol_at(size_t) const override { return true; }
    [[nodiscard]] Symbol symbol_at(size_t row) const override { return row % 4 == 0 ? Symbol::Folder : Symbol::File; }
};

struct TreeItem {
    StringView title;
    Vector<TreeItem*> children;
};

class TreeModel final : public OutlineModel {
public:
    TreeModel()
    {
        m_root.children.append(&m_documents);
        m_root.children.append(&m_pictures);
        m_root.children.append(&m_music);
        m_documents.children.append(&m_letters);
        m_documents.children.append(&m_notes);
        m_letters.children.append(&m_first);
        m_pictures.children.append(&m_holiday);
    }
    [[nodiscard]] size_t child_count(void* parent) const override { return item(parent).children.size(); }
    [[nodiscard]] void* child_at(void* parent, size_t index) const override { return item(parent).children[index]; }
    [[nodiscard]] String text_of(void* entry, size_t) const override { return MUST(String::from_view(static_cast<TreeItem*>(entry)->title)); }
    [[nodiscard]] bool has_symbol(void*) const override { return true; }
    [[nodiscard]] Symbol symbol_of(void* entry) const override { return static_cast<TreeItem*>(entry)->children.is_empty() ? Symbol::File : Symbol::Folder; }
    [[nodiscard]] TreeItem* documents() { return &m_documents; }

private:
    [[nodiscard]] TreeItem const& item(void* parent) const { return parent ? *static_cast<TreeItem*>(parent) : m_root; }
    TreeItem m_root { {}, {} };
    TreeItem m_documents { "Documents"_sv, {} };
    TreeItem m_pictures { "Pictures"_sv, {} };
    TreeItem m_music { "Music"_sv, {} };
    TreeItem m_letters { "Letters"_sv, {} };
    TreeItem m_notes { "Notes"_sv, {} };
    TreeItem m_first { "First letter"_sv, {} };
    TreeItem m_holiday { "Holiday"_sv, {} };
};

class ItemsModel final : public CollectionModel {
public:
    [[nodiscard]] size_t item_count() const override { return 14; }
    [[nodiscard]] String title_of(size_t index) const override
    {
        StringBuilder builder;
        builder.appendff("Item {}", index + 1);
        return MUST(builder.to_string());
    }
    [[nodiscard]] Symbol symbol_of(size_t index) const override { return index % 3 == 0 ? Symbol::Folder : Symbol::File; }
};

FilesModel s_files;
TreeModel s_tree;
ItemsModel s_items;

void populate_collections(Page& page)
{
    auto& top = page.add<SplitView>();
    top.set_stretch(0, 1);
    auto& table = top.add_pane<TableView>(520, 200, &s_files);
    table.add_column("Name"_sv, 240);
    table.add_column("Size"_sv, 90);
    table.add_column("Kind"_sv, 140);
    table.set_alternating_rows(true);
    table.set_allows_multiple_selection(true);
    table.select_row(2);
    auto& outline = top.add_pane<OutlineView>(300, 160, s_tree);
    outline.add_column("Tree"_sv, 240);
    outline.set_expanded(s_tree.documents(), true);
    outline.select_row(1);
    auto& scroll = page.add<ScrollView>();
    scroll.set_fixed_height(214);
    scroll.set_scrolls_horizontally(false);
    auto& collection = scroll.make_document<CollectionView>(s_items);
    collection.select_item(4);
}

void populate_containers(Page& page)
{
    auto& tabs = page.add<TabView>();
    tabs.set_fixed_height(190);
    auto& first = tabs.add_tab<StackView>("General"_sv, Axis::Vertical, 8, Alignment::Leading, Insets::all(12));
    (void)first.add<Label>("The first tab's contents."_sv);
    (void)first.add<Checkbox>("With a checkbox"_sv, ToggleState::On);
    (void)first.add<Button>("And a button"_sv);
    auto& second = tabs.add_tab<StackView>("Advanced"_sv, Axis::Vertical, 8, Alignment::Fill, Insets::all(12));
    (void)second.add<Label>("The second."_sv);
    (void)tabs.add_tab<View>("Sharing"_sv);

    auto& split = page.add<SplitView>(Axis::Horizontal, DividerStyle::Thick);
    split.set_fixed_height(160);
    auto& left = split.add_pane<Box>(260, 100, "Left"_sv);
    left.content().set_layout<StackLayout>(Axis::Vertical, 6);
    (void)left.content().add<Label>("A box, with a title."_sv);
    (void)split.add_pane<Box>(0, 100, "Right"_sv);

    (void)page.add<Separator>();

    auto& form = page.add<View>();
    auto& grid = form.set_layout<GridLayout>(2, 8, 8);
    grid.set_first_column_trailing(true);
    (void)form.add<Label>("Name:"_sv);
    auto& name = form.add<TextField>("Ada Lovelace"_sv);
    name.set_stretch(1, 0);
    (void)form.add<Label>("Language:"_sv);
    auto& language = form.add<PopUpButton>();
    language.add_item("English"_sv);
    language.add_item("Tamil"_sv);
    (void)form.add<Label>("Notify:"_sv);
    (void)form.add<Switch>(true);
    auto& rest = page.add<View>();
    rest.set_stretch(0, 1);
}

// The builder's page: the same kinds of thing, said shortly.
NonnullOwnPtr<View> built_page(Function<void()> on_alert, Function<void()> on_menu, Function<void()> on_popover, Function<void()> on_panel)
{
    StringView choices[3] = { "Small"_sv, "Medium"_sv, "Large"_sv };
    return vstack(12,
        label("Made with the builder"_sv, "title2"_sv),
        label("These open what comes over a window."_sv).with([](Label& text) { text.set_style(LabelStyle::Secondary); }),
        hstack(12,
            button("Alert…"_sv).action(move(on_alert)),
            button("Menu…"_sv).action(move(on_menu)),
            button("Popover…"_sv).action(move(on_popover)),
            default_button("Open…"_sv).action(move(on_panel)),
            spacer()),
        separator(),
        hstack(12, label("Size:"_sv), segments(Span<StringView const>(choices, 3), 1), spacer()),
        hstack(12, label("Fit:"_sv), popup(Span<StringView const>(choices, 3), 2), spacer()),
        hstack(12, checkbox("Remember this"_sv, true), toggle(true), slider(0, 1, 0.5f).width(160), spacer()),
        box("A group"_sv, label("Inside the box."_sv), checkbox("Inside too"_sv)).height(96),
        spacer())
        .with([](StackView& stack) { stack.stack().set_insets({ 20, 24, 20, 24 }); })
        .release();
}

class GalleryRoot final : public View {
public:
    void draw(Graphics& graphics) override
    {
        // Only the content's side: the sidebar's is the material.
        auto const& t = Appearance::the();
        float sidebar = t.metric("sidebar.width"_sv);
        graphics.fill({ sidebar, 0, width() - sidebar, height() }, t.color("windowBackground"_sv));
    }
};

class Gallery final : public Responder {
public:
    Gallery()
    {
        auto const& t = Appearance::the();
        m_window = adopt_own(*new UI::Window("Widget Gallery"_sv, { 40, 36, 1180, 720 }, WS::WindowTitled | WS::WindowResizable | WS::WindowUnifiedTitle));
        m_window->set_transparent(true);
        m_window->set_sheet_top(t.metric("window.toolbarUnified"_sv));
        auto& root = m_window->set_content<GalleryRoot>();
        (void)root;
        auto& split = root.add<SplitView>();
        float sidebar_width = t.metric("sidebar.width"_sv);
        m_sidebar = &split.add_pane<Sidebar>(sidebar_width, sidebar_width);
        m_sidebar->set_top_padding(t.metric("window.toolbarCompact"_sv));
        auto& right = split.add_pane<StackView>(0, 300, Axis::Vertical);
        m_toolbar = &right.add<Toolbar>();
        m_toolbar->set_left_padding(t.metric("toolbar.paddingRight"_sv));
        m_toolbar->set_title("Widget Gallery"_sv);
        m_toolbar->add_flexible_space();
        (void)m_toolbar->add_button(Symbol::Sidebar, [] {}, "Sidebar"_sv);
        (void)m_toolbar->add_search(160);
        m_pages = &right.add<View>();
        m_pages->set_stretch(0, 1);
        split.set_frame({ 0, 0, 1180, 720 });
        m_split = &split;

        m_sidebar->add_section("Controls"_sv);
        m_sidebar->add_row("Buttons"_sv, Symbol::Checkmark);
        m_sidebar->add_row("Values"_sv, Symbol::ChevronUpDown);
        m_sidebar->add_row("Text"_sv, Symbol::File);
        m_sidebar->add_section("Views"_sv);
        m_sidebar->add_row("Collections"_sv, Symbol::Folder);
        m_sidebar->add_row("Containers"_sv, Symbol::Sidebar);
        m_sidebar->add_row("Builder"_sv, Symbol::Plus);
        m_sidebar->on_select = [this](size_t row) { show_page(row); };

        populate_buttons(m_pages->add<Page>());
        populate_values(m_pages->add<Page>());
        populate_text(m_pages->add<Page>());
        populate_collections(m_pages->add<Page>());
        populate_containers(m_pages->add<Page>());
        m_pages->adopt_child(built_page([this] { show_alert(); }, [this] { show_menu(); }, [this] { show_popover(); }, [this] { show_open_panel(); }));
        for (size_t i = 0; i < m_pages->children().size(); ++i)
            m_pages->children()[i]->set_hidden(true);
        m_pages->set_layout<PagesLayout>();
        m_window->on_key_change = [this](bool active) {
            (void)active;
            update_material();
        };
        m_window->on_resize = [this](FloatSize size) {
            m_split->set_frame({ 0, 0, size.width, size.height });
            update_material();
        };
        update_material();
        m_sidebar->select_row(0);
        // Whatever else the window gets, the keys of the gallery.
        m_window->set_key_interceptor(this);
        m_window->show();
    }

    [[nodiscard]] UI::Window& window() { return *m_window; }

    bool key_down(KeyEvent const& event) override
    {
        // A field that has the keyboard wants letters.
        auto* first = m_window->first_responder();
        if (first && first != m_sidebar && !event.has_command() && (first->can_perform_command("paste"_sv) || m_field_active))
            return false;
        if (event.has_command())
            return false;
        switch (event.code) {
        case Key1:
        case Key1 + 1:
        case Key1 + 2:
        case Key1 + 3:
        case Key1 + 4:
        case Key1 + 5:
            m_sidebar->select_row(event.code - Key1);
            announce();
            return true;
        case KeyD:
            MUST(Application::the().connection().server().set_appearance(Appearance::the().is_dark() ? "Light"_sv : "Dark"_sv));
            announce();
            return true;
        case KeyM:
            show_menu();
            announce();
            return true;
        case KeyS:
            show_alert();
            announce();
            return true;
        case KeyP:
            show_popover();
            announce();
            return true;
        case KeyO:
            show_open_panel();
            announce();
            return true;
        case KeyC:
            ColorPanel::the().show(Color { 0, 122, 255, 255 }, nullptr);
            announce();
            return true;
        case KeyF:
            FontPanel::the().show(FontSpec { 13, FontWeight::Regular, false }, nullptr);
            announce();
            return true;
        default:
            return false;
        }
    }

    bool key_up(KeyEvent const&) override { return false; }

    void announce()
    {
        // Once what it changed is on screen, for those watching the console.
        Application::the().event_loop().add_timer(400, false, [this] {
            printf("gallery: page %d, %s\n", m_sidebar->selected_row() + 1, Appearance::the().is_dark() ? "dark" : "light");
            fflush(stdout);
        });
    }

    void show_page(size_t index)
    {
        for (size_t i = 0; i < m_pages->children().size(); ++i)
            m_pages->children()[i]->set_hidden(i != index);
        m_pages->set_needs_layout();
    }

private:
    class PagesLayout final : public Layout {
    public:
        void apply(View& view) override
        {
            for (auto const& child : view.children())
                child->set_frame({ 0, 0, view.width(), view.height() });
        }
        [[nodiscard]] FloatSize preferred_size(View const&) const override { return { 0, 0 }; }
    };

    void update_material()
    {
        auto const& t = Appearance::the();
        MaterialRegion region { { 0, 0, static_cast<int>(t.metric("sidebar.width"_sv)), static_cast<int>(m_window->size().height) }, WS::Material::Sidebar };
        m_window->set_materials(Span<MaterialRegion const>(&region, 1));
    }

    void show_alert()
    {
        Alert alert("Save the changes to this document?"_sv, "They will be lost if you close it without saving."_sv);
        alert.add_button("Save"_sv);
        alert.add_button("Cancel"_sv);
        m_window->set_key_interceptor(nullptr);
        alert.show(*m_window, [this](size_t) { m_window->set_key_interceptor(this); });
    }

    void show_menu()
    {
        if (!m_menu) {
            m_menu = adopt_own(*new Menu);
            m_menu->add_with_shortcut("Cut"_sv, KeyX, "\xE2\x8C\x98X"_sv, nullptr);
            m_menu->add_with_shortcut("Copy"_sv, KeyC, "\xE2\x8C\x98" "C"_sv, nullptr);
            auto& paste = m_menu->add_with_shortcut("Paste"_sv, KeyV, "\xE2\x8C\x98V"_sv, nullptr);
            paste.enabled = false;
            m_menu->add_separator();
            auto& checked = m_menu->add("Show Sidebar"_sv);
            checked.checked = true;
            auto& more = m_menu->add_submenu("More"_sv);
            more.add("First"_sv);
            more.add("Second"_sv);
        }
        auto frame = m_window->frame();
        m_menu->on_close = [this] { m_window->set_key_interceptor(this); };
        m_menu->open(*m_window, { frame.x + 300, frame.y + 140 });
        m_menu->set_highlight(1);
    }

    void show_popover()
    {
        auto const& t = Appearance::the();
        float padding = t.metric("popover.padding"_sv);
        auto* content = new StackView(Axis::Vertical, 8, Alignment::Fill, Insets::all(padding));
        (void)content->add<Label>("A popover"_sv, t.named_font("headline"_sv));
        (void)content->add<Label>("It closes on a click elsewhere."_sv);
        (void)content->add<Slider>(0.0f, 1.0f, 0.6f);
        m_popover.on_close = [this] { m_window->set_key_interceptor(this); };
        m_popover.show(*m_window, adopt_own(*static_cast<View*>(content)), { 240, 110 }, { 400, 60, 40, 24 });
    }

    void show_open_panel()
    {
        m_window->set_key_interceptor(nullptr);
        FilePanel::run(*m_window, FilePanel::Kind::Open, "/"_sv, {}, [this](StringView) { m_window->set_key_interceptor(this); });
    }

    bool m_field_active { false };
    OwnPtr<UI::Window> m_window;
    OwnPtr<Menu> m_menu;
    Popover m_popover;
    View* m_split { nullptr };
    Sidebar* m_sidebar { nullptr };
    Toolbar* m_toolbar { nullptr };
    View* m_pages { nullptr };
};

}

int main(int argc, char** argv)
{
    bool still = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--still") == 0)
            still = true;
    }
    auto app_or_error = Application::create("Widget Gallery"_sv, "com.vulture.WidgetGallery"_sv);
    if (app_or_error.is_error()) {
        fprintf(stderr, "WidgetGallery: cannot reach the window server\n");
        return 1;
    }
    auto app = app_or_error.release_value();
    if (still)
        set_motion_enabled(false);
    Gallery gallery;
    gallery.window().on_closed = [&] { app->quit(); };
    gallery.announce();
    return app->exec();
}
