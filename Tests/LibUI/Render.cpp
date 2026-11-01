/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Render.h"
#include "TestPlatform.h"
#include <LibJSON/JSON.h>
#include <LibUI/Appearance.h>
#include <LibUI/Motion.h>
#include <LibUI/Window.h>

using namespace Gfx;

static Look const s_looks[4] = {
    { "light-1x"_sv, "Light"_sv, 1, "light-1x"_sv },
    { "light-2x"_sv, "Light"_sv, 2, "light-2x"_sv },
    { "dark-1x"_sv, "Dark"_sv, 1, "dark-1x"_sv },
    { "dark-2x"_sv, "Dark"_sv, 2, "dark-2x"_sv },
};

Span<Look const> all_looks()
{
    return { s_looks, 4 };
}

HashMap<String, MeasuredControl> read_measured_controls()
{
    HashMap<String, MeasuredControl> controls;
    auto bytes = MUST(TestPlatform::the().read_file(VULTURE_MEASURED_DIRECTORY "/controls.json"_sv));
    auto json = MUST(JSON::parse(StringView(reinterpret_cast<char const*>(bytes.data()), bytes.size())));
    auto number = [](JSON::Value const& object, StringView key) {
        return static_cast<float>(object.get_number(key).value_or(0));
    };
    auto read_box = [&](JSON::Value const& object, StringView key) -> MeasuredBox {
        auto const* box = object.get_object(key);
        if (!box)
            return {};
        return { static_cast<int>(number(*box, "x"_sv)), static_cast<int>(number(*box, "y"_sv)), static_cast<int>(number(*box, "w"_sv)), static_cast<int>(number(*box, "h"_sv)) };
    };
    for (size_t i = 0; i < json.size(); ++i) {
        auto const& entry = json.at(i);
        MeasuredControl control;
        auto const& frame = *entry.get_object("frame"_sv);
        control.frame_width = number(frame, "w"_sv);
        control.frame_height = number(frame, "h"_sv);
        auto const& intrinsic = *entry.get_object("intrinsic"_sv);
        control.intrinsic_width = number(intrinsic, "w"_sv);
        control.intrinsic_height = number(intrinsic, "h"_sv);
        auto const& insets = *entry.get_object("alignmentInsets"_sv);
        control.inset_top = number(insets, "top"_sv);
        control.inset_left = number(insets, "left"_sv);
        control.inset_bottom = number(insets, "bottom"_sv);
        control.inset_right = number(insets, "right"_sv);
        for (size_t look = 0; look < 4; ++look) {
            if (auto const* one = entry.get_object(s_looks[look].key)) {
                control.box[look] = read_box(*one, "box"_sv);
                control.soft_box[look] = read_box(*one, "softBox"_sv);
            }
        }
        controls.set(MUST(String::from_view(json.key_at(i))), control);
    }
    return controls;
}

// Where it goes in AppKit's frame. Mostly that is the frame less its alignment insets: a
// frame here is an alignment rectangle. A few controls are not drawn where AppKit's own
// insets say.
static FloatRect placement_of(StringView name, MeasuredControl const& numbers, FloatSize preferred)
{
    FloatRect place { numbers.inset_left, numbers.inset_top, numbers.frame_width - numbers.inset_left - numbers.inset_right, numbers.frame_height - numbers.inset_top - numbers.inset_bottom };
    // Its insets take more than its frame has.
    if (place.height <= 0)
        place = { 0, 0, numbers.frame_width, numbers.frame_height };
    // Combo boxes state insets, but fill their frame from top to bottom.
    if (name.starts_with("combo."_sv))
        place = { numbers.inset_left, 0, numbers.frame_width - numbers.inset_left - numbers.inset_right, numbers.frame_height };
    // A switch of a small size stays in the frame of a large one, centered, rounded down.
    if (name.starts_with("switch."_sv) && (name.ends_with(".small"_sv) || name.ends_with(".mini"_sv))) {
        place = { static_cast<float>(static_cast<int>((numbers.frame_width - preferred.width) / 2)), static_cast<float>(static_cast<int>((numbers.frame_height - preferred.height - 1) / 2)), preferred.width, preferred.height };
    }
    // A legacy scroller is a point narrower than its frame, and on the right.
    if (name == "scroller.legacy"_sv)
        place = { 1, 0, numbers.frame_width - 1, numbers.frame_height };
    // These sit a point away from where their insets put them, in AppKit.
    if (name == "checkbox.off.mini"_sv)
        place.y += 1;
    if (name == "radio.off.mini"_sv)
        place.y -= 1;
    if (name == "radio.off.small"_sv)
        place.y -= 1;
    return place;
}

Rendered render_entry(CatalogEntry const& entry, Look const& look, HashMap<String, MeasuredControl> const& measured)
{
    auto& platform = TestPlatform::the();
    platform.set_scale(look.scale);
    UI::set_motion_enabled(false);
    MUST(UI::Appearance::use(look.appearance));

    auto const* numbers = measured.get(entry.name);
    VERIFY(numbers);
    // A margin on all sides, with all the frame's rows and more.
    constexpr int margin = 24;
    int canvas_width = static_cast<int>(numbers->frame_width) + margin * 2;
    int canvas_height = static_cast<int>(numbers->frame_height) + margin * 2;
    UI::Window window("catalog"_sv, { 0, 0, canvas_width, canvas_height }, UI::WS::WindowBare | UI::WS::WindowHidden);
    window.set_transparent(true);
    window.handle_activation(true);

    auto view = entry.make();
    Rendered result;
    auto preferred = view->intrinsic_size();
    result.intrinsic = { static_cast<int>(preferred.width), static_cast<int>(preferred.height) };
    auto& placed = *view;
    auto place = placement_of(entry.name, *numbers, preferred);
    placed.set_frame({ static_cast<float>(margin) + place.x, static_cast<float>(margin) + place.y, place.width, place.height });
    window.root_view().adopt_child(move(view));
    window.show();
    auto& pixels = shown_pixels(window);

    int frame_x = margin * look.scale;
    int frame_y = margin * look.scale;
    int frame_width = static_cast<int>(numbers->frame_width) * look.scale;
    int frame_height = static_cast<int>(numbers->frame_height) * look.scale;

    auto box_at = [&](int threshold) -> IntRect {
        int left = 1 << 30, top = 1 << 30, right = -1, bottom = -1;
        for (int y = 0; y < pixels.height(); ++y) {
            for (int x = 0; x < pixels.width(); ++x) {
                auto pixel = pixels.pixel_at(x, y);
                int most = max(max(static_cast<int>(pixel_red(pixel)), static_cast<int>(pixel_green(pixel))), max(static_cast<int>(pixel_blue(pixel)), static_cast<int>(pixel_alpha(pixel))));
                if (most <= threshold)
                    continue;
                left = min(left, x);
                top = min(top, y);
                right = max(right, x);
                bottom = max(bottom, y);
            }
        }
        if (right < 0)
            return {};
        return { left - frame_x, top - frame_y, right - left + 1, bottom - top + 1 };
    };
    result.box = box_at(32);
    result.soft_box = box_at(2);

    int middle_x = frame_x + frame_width / 2;
    int middle_y = frame_y + frame_height / 2;
    for (int y = frame_y - 8 * look.scale; y < frame_y + frame_height + 8 * look.scale; ++y)
        result.down.append(pixels.pixel_at(middle_x, y));
    for (int x = frame_x - 8 * look.scale; x < frame_x + frame_width + 8 * look.scale; ++x)
        result.across.append(pixels.pixel_at(x, middle_y));
    result.picture = MUST(pixels.cropped({ frame_x - 12 * look.scale, frame_y - 12 * look.scale, frame_width + 24 * look.scale, frame_height + 24 * look.scale }));
    return result;
}
