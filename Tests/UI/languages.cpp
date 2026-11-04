/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/Controls.h>
#include <LibUI/Layout.h>
#include <LibUI/Menu.h>
#include <LibUI/ScrollView.h>
#include <LibUI/TableView.h>
#include <LibUI/TextField.h>
#include <LibUI/TextView.h>
#include <stdio.h>

// The world's writing, in every kind of control: in a title bar, fields, a table, menus, and rich text. For Tests/UI/Milestones/m18-languages.chat to look at, and to type into.

using namespace UI;

namespace {

struct Sample {
    StringView language;
    StringView text;
};

constexpr Sample s_samples[] = {
    { "Arabic"_sv, "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85"_sv },
    { "Hebrew"_sv, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d"_sv },
    { "Hindi"_sv, "\xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87 \xe0\xa4\xa6\xe0\xa5\x81\xe0\xa4\xa8\xe0\xa4\xbf\xe0\xa4\xaf\xe0\xa4\xbe"_sv },
    { "Chinese"_sv, "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c"_sv },
    { "Japanese"_sv, "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf\xe4\xb8\x96\xe7\x95\x8c"_sv },
    { "Korean"_sv, "\xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94"_sv },
    { "Thai"_sv, "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5"_sv },
    { "Tamil"_sv, "\xe0\xae\xb5\xe0\xae\xa3\xe0\xae\x95\xe0\xaf\x8d\xe0\xae\x95\xe0\xae\xae\xe0\xaf\x8d"_sv },
    { "Emoji"_sv, "\xf0\x9f\x98\x80\xf0\x9f\x8e\x89\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5"_sv },
    { "Mixed"_sv, "abc \xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a 123 \xe4\xb8\xad\xe6\x96\x87"_sv },
};

class SampleModel final : public TableModel {
public:
    [[nodiscard]] size_t row_count() const override { return array_size(s_samples); }
    [[nodiscard]] String text_at(size_t row, size_t column) const override
    {
        return MUST(String::from_view(column == 0 ? s_samples[row].language : s_samples[row].text));
    }
};

}

int main()
{
    auto app_or_error = Application::create("Languages"_sv, "com.vulture.test.languages"_sv);
    if (app_or_error.is_error())
        return 1;
    auto app = app_or_error.release_value();

    UI::Window window("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87 \xe4\xbd\xa0\xe5\xa5\xbd \xf0\x9f\x98\x80"_sv, { 120, 90, 820, 620 }, ::Window::WindowTitled | ::Window::WindowResizable);
    auto& stack = window.set_content<StackView>(Axis::Vertical, 8, Alignment::Fill, Insets { 12, 12, 12, 12 });

    // A field for each.
    TextField* typing_field = nullptr;
    for (auto const& sample : s_samples) {
        auto& row = stack.add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& label = row.add<Label>(sample.language);
        label.set_fixed_width(70);
        auto& field = row.add<TextField>(sample.text);
        field.set_accessibility_label(sample.language);
        field.set_stretch(1, 0);
    }
    {
        auto& row = stack.add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& label = row.add<Label>("Type here"_sv);
        label.set_fixed_width(70);
        auto& field = row.add<TextField>(""_sv);
        field.set_accessibility_label("Type here"_sv);
        field.set_stretch(1, 0);
        typing_field = &field;
    }

    // A menu of them.
    auto& popup_row = stack.add<StackView>(Axis::Horizontal, 8, Alignment::Center);
    auto& popup = popup_row.add<PopUpButton>();
    popup.set_accessibility_label("Languages"_sv);
    for (auto const& sample : s_samples)
        popup.add_item(sample.text);

    // A table of them.
    SampleModel model;
    auto& table = stack.add<TableView>(&model);
    table.add_column("Language"_sv, 100);
    table.add_column("\xd9\x86\xd8\xb5 \xe6\x96\x87\xe6\x9c\xac"_sv, 300);
    table.set_stretch(1, 1);

    // Styles, sizes and directions, all at once.
    auto& rich = stack.add<TextView>();
    rich.set_rich(true);
    rich.set_text("Bold, italic and underlined. \xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a \xe0\xa4\xb9\xe0\xa4\xbf\xe0\xa4\xa8\xe0\xa5\x8d\xe0\xa4\xa6\xe0\xa5\x80 \xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x98\x80 and something large."_sv);
    rich.storage().select(0, 4);
    rich.storage().change_attributes([](TextAttributes& attributes) { attributes.bold = true; });
    rich.storage().select(6, 12);
    rich.storage().change_attributes([](TextAttributes& attributes) { attributes.italic = true; });
    rich.storage().select(17, 27);
    rich.storage().change_attributes([](TextAttributes& attributes) { attributes.underline = true; });
    rich.storage().select(rich.storage().length() - 6, rich.storage().length() - 1);
    rich.storage().change_attributes([](TextAttributes& attributes) { attributes.size = 22; });
    rich.storage().select(0, 0);
    rich.set_fixed_height(70);

    // The menu bar.
    auto menus = app->standard_menus();
    app->set_main_menu(move(menus));

    window.on_closed = [&] { app->quit(); };
    window.show();
    (void)window.make_first_responder(typing_field);
    puts("languages: ready");
    return app->exec();
}
