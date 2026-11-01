/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Catalog.h"
#include <LibUI/Controls.h>
#include <LibUI/Menu.h>
#include <LibUI/ScrollView.h>
#include <LibUI/TableView.h>
#include <LibUI/TextField.h>

using namespace UI;

namespace {

template<typename T>
NonnullOwnPtr<View> own(T* view)
{
    return adopt_own(*static_cast<View*>(view));
}

template<ControlSize size>
NonnullOwnPtr<View> push_button()
{
    auto* button = new Button("Button"_sv);
    button->set_control_size(size);
    return own(button);
}

template<ControlSize size>
NonnullOwnPtr<View> checkbox_off()
{
    auto* box = new Checkbox;
    box->set_control_size(size);
    return own(box);
}

template<ControlSize size>
NonnullOwnPtr<View> radio_off()
{
    auto* radio = new RadioButton;
    radio->set_control_size(size);
    return own(radio);
}

template<ControlSize size>
NonnullOwnPtr<View> popup()
{
    auto* button = new PopUpButton;
    button->add_item("Popup"_sv);
    button->set_control_size(size);
    return own(button);
}

template<ControlSize size>
NonnullOwnPtr<View> segmented()
{
    auto* control = new SegmentedControl;
    control->add_segment("One"_sv);
    control->add_segment("Two"_sv);
    control->add_segment("Three"_sv);
    control->set_selected(1);
    control->set_control_size(size);
    return own(control);
}

template<ControlSize size>
NonnullOwnPtr<View> stepper()
{
    auto* control = new Stepper;
    control->set_control_size(size);
    return own(control);
}

template<ControlSize size, bool on>
NonnullOwnPtr<View> a_switch()
{
    auto* control = new Switch(on);
    control->set_control_size(size);
    return own(control);
}

template<ControlSize size>
NonnullOwnPtr<View> slider()
{
    auto* control = new Slider(0, 100, 40);
    control->set_control_size(size);
    return own(control);
}

template<ControlSize size>
NonnullOwnPtr<View> field()
{
    auto* control = new TextField("Text"_sv);
    control->set_control_size(size);
    return own(control);
}

template<ControlSize size>
NonnullOwnPtr<View> search()
{
    auto* control = new SearchField;
    control->set_control_size(size);
    return own(control);
}

template<ControlSize size>
NonnullOwnPtr<View> combo()
{
    auto* control = new ComboBox;
    control->set_text("Combo"_sv);
    control->set_control_size(size);
    return own(control);
}

template<ControlSize size>
NonnullOwnPtr<View> progress()
{
    auto* control = new ProgressBar(0.4f);
    control->set_control_size(size);
    return own(control);
}

NonnullOwnPtr<View> styled_button(ButtonStyle style, StringView title, bool enabled = true)
{
    auto* button = new Button(title, style);
    button->set_enabled(enabled);
    return own(button);
}

}

Vector<CatalogEntry> control_catalog()
{
    using enum ControlSize;
    Vector<CatalogEntry> list;
    auto add = [&](StringView name, NonnullOwnPtr<View> (*make)()) { list.append(CatalogEntry { name, make }); };

    add("button.large"_sv, push_button<Large>);
    add("button.regular"_sv, push_button<Regular>);
    add("button.small"_sv, push_button<Small>);
    add("button.mini"_sv, push_button<Mini>);
    add("checkbox.off.large"_sv, checkbox_off<Large>);
    add("checkbox.off.regular"_sv, checkbox_off<Regular>);
    add("checkbox.off.small"_sv, checkbox_off<Small>);
    add("checkbox.off.mini"_sv, checkbox_off<Mini>);
    add("radio.off.large"_sv, radio_off<Large>);
    add("radio.off.regular"_sv, radio_off<Regular>);
    add("radio.off.small"_sv, radio_off<Small>);
    add("radio.off.mini"_sv, radio_off<Mini>);
    add("popup.large"_sv, popup<Large>);
    add("popup.regular"_sv, popup<Regular>);
    add("popup.small"_sv, popup<Small>);
    add("popup.mini"_sv, popup<Mini>);
    add("segmented.large"_sv, segmented<Large>);
    add("segmented.regular"_sv, segmented<Regular>);
    add("segmented.small"_sv, segmented<Small>);
    add("segmented.mini"_sv, segmented<Mini>);
    add("stepper.large"_sv, stepper<Large>);
    add("stepper.regular"_sv, stepper<Regular>);
    add("stepper.small"_sv, stepper<Small>);
    add("stepper.mini"_sv, stepper<Mini>);
    add("switch.on.large"_sv, a_switch<Large, true>);
    add("switch.on.regular"_sv, a_switch<Regular, true>);
    add("switch.on.small"_sv, a_switch<Small, true>);
    add("switch.on.mini"_sv, a_switch<Mini, true>);
    add("switch.off.large"_sv, a_switch<Large, false>);
    add("switch.off.regular"_sv, a_switch<Regular, false>);
    add("switch.off.small"_sv, a_switch<Small, false>);
    add("switch.off.mini"_sv, a_switch<Mini, false>);
    add("slider.large"_sv, slider<Large>);
    add("slider.regular"_sv, slider<Regular>);
    add("slider.small"_sv, slider<Small>);
    add("slider.mini"_sv, slider<Mini>);
    add("field.large"_sv, field<Large>);
    add("field.regular"_sv, field<Regular>);
    add("field.small"_sv, field<Small>);
    add("field.mini"_sv, field<Mini>);
    add("search.large"_sv, search<Large>);
    add("search.regular"_sv, search<Regular>);
    add("search.small"_sv, search<Small>);
    add("search.mini"_sv, search<Mini>);
    add("combo.large"_sv, combo<Large>);
    add("combo.regular"_sv, combo<Regular>);
    add("combo.small"_sv, combo<Small>);
    add("combo.mini"_sv, combo<Mini>);
    add("progress.large"_sv, progress<Large>);
    add("progress.regular"_sv, progress<Regular>);
    add("progress.small"_sv, progress<Small>);
    add("progress.mini"_sv, progress<Mini>);

    add("button.default"_sv, [] { return styled_button(ButtonStyle::Default, "Button"_sv); });
    add("button.disabled"_sv, [] { return styled_button(ButtonStyle::Push, "Button"_sv, false); });
    add("button.textured"_sv, [] { return styled_button(ButtonStyle::Textured, "Button"_sv); });
    add("button.help"_sv, [] { return styled_button(ButtonStyle::Help, {}); });
    add("button.disclosure"_sv, [] { return styled_button(ButtonStyle::Disclosure, {}); });
    add("button.roundedDisclosure"_sv, [] { return styled_button(ButtonStyle::RoundedDisclosure, {}); });
    add("button.circular"_sv, [] { return styled_button(ButtonStyle::Circular, {}); });
    add("checkbox.on"_sv, [] { return own(new Checkbox({}, ToggleState::On)); });
    add("checkbox.mixed"_sv, [] { return own(new Checkbox({}, ToggleState::Mixed)); });
    add("checkbox.disabled"_sv, [] {
        auto* box = new Checkbox({}, ToggleState::On);
        box->set_enabled(false);
        return own(box);
    });
    add("radio.on"_sv, [] { return own(new RadioButton({}, true)); });
    add("slider.ticks"_sv, [] {
        auto* control = new Slider(0, 100, 40);
        control->set_tick_count(11);
        return own(control);
    });
    add("slider.vertical"_sv, [] {
        auto* control = new Slider(0, 100, 40);
        control->set_vertical(true);
        return own(control);
    });
    add("slider.disabled"_sv, [] {
        auto* control = new Slider(0, 100, 40);
        control->set_enabled(false);
        return own(control);
    });
    add("popup.pulldown"_sv, [] {
        auto* button = new PopUpButton(true);
        button->set_title("Pull"_sv);
        button->add_item("Item"_sv);
        return own(button);
    });
    add("field.disabled"_sv, [] {
        auto* control = new TextField("Text"_sv);
        control->set_enabled(false);
        return own(control);
    });
    add("field.placeholder"_sv, [] {
        auto* control = new TextField;
        control->set_placeholder("Text"_sv);
        return own(control);
    });
    add("field.rounded"_sv, [] { return own(new TextField("Text"_sv, FieldStyle::Rounded)); });
    add("spinner"_sv, [] { return own(new Spinner); });
    add("box"_sv, [] { return own(new Box("Box"_sv)); });
    add("colorwell"_sv, [] { return own(new ColorWell); });
    add("level"_sv, [] { return own(new LevelIndicator(0.4f)); });
    add("separator"_sv, [] { return own(new Separator); });
    add("scroller.overlay"_sv, [] {
        auto* scroller = new Scroller(Axis::Vertical, ScrollerStyle::Overlay);
        scroller->set_knob(0.3f, 0.4f);
        scroller->set_revealed(true);
        return own(scroller);
    });
    add("scroller.legacy"_sv, [] {
        auto* scroller = new Scroller(Axis::Vertical, ScrollerStyle::Legacy);
        scroller->set_knob(0.3f, 0.4f);
        return own(scroller);
    });
    add("table.header"_sv, [] {
        auto* header = new TableHeaderView;
        header->add_column("Name"_sv, 160);
        return own(header);
    });
    return list;
}
