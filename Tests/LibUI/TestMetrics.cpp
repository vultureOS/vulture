/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Catalog.h"
#include "Render.h"
#include "TestPlatform.h"
#include <LibTest/TestCase.h>
#include <LibUI/Appearance.h>

extern "C" int printf(char const* format, ...);

// Every control against what AppKit measured (Tests/UI/Measured): the box of what can
// be seen, in light and dark, at 1x and 2x, to the pixel on each edge.

namespace {

// Where the box is set by the label's ends (the face is too faint to count), and Inter
// is wider than SF: the left and right edges tell nothing.
bool ends_are_text(StringView name, UI::Appearance const&)
{
    return name.starts_with("segmented."_sv) || name == "box"_sv || name == "button.textured"_sv || name == "table.header"_sv;
}

// What clears the threshold here is a face sitting right at it, and nothing to go by.
bool is_at_the_threshold(StringView name, bool dark)
{
    if (!dark)
        return false;
    return name == "button.disabled"_sv || name == "checkbox.disabled"_sv || name == "box"_sv || name == "scroller.legacy"_sv;
}

}

TEST_CASE(what_shows_of_every_control_is_within_a_pixel_of_appkit)
{
    (void)TestPlatform::the();
    auto measured = read_measured_controls();
    size_t compared = 0, off = 0;
    for (auto const& entry : control_catalog()) {
        // AppKit draws its overlay scroller's resting state, in which no knob shows; ours
        // is revealed, to have something to see. Nothing to compare.
        if (entry.name == "scroller.overlay"_sv)
            continue;
        auto const* numbers = measured.get(entry.name);
        EXPECT(numbers != nullptr);
        auto looks = all_looks();
        for (size_t i = 0; i < looks.size(); ++i) {
            auto rendered = render_entry(entry, looks[i], measured);
            auto const& box = numbers->box[i];
            if (is_at_the_threshold(entry.name, looks[i].appearance == "Dark"_sv))
                continue;
            int edges[4] = { rendered.box.x - box.x, rendered.box.y - box.y, rendered.box.right() - (box.x + box.width), rendered.box.bottom() - (box.y + box.height) };
            if (ends_are_text(entry.name, UI::Appearance::the()))
                edges[0] = edges[2] = 0;
            // An off switch's far end sits just at the threshold, in the light.
            if (entry.name.starts_with("switch.off."_sv) && looks[i].appearance == "Light"_sv)
                edges[2] = 0;
            ++compared;
            for (int edge : edges) {
                if (edge > 1 || edge < -1) {
                    ++off;
                    printf("%.*s %.*s: box (%d,%d %dx%d), AppKit (%d,%d %dx%d)\n", static_cast<int>(entry.name.length()), entry.name.characters_without_null_termination(),
                        static_cast<int>(looks[i].name.length()), looks[i].name.characters_without_null_termination(),
                        rendered.box.x, rendered.box.y, rendered.box.width, rendered.box.height, box.x, box.y, box.width, box.height);
                    break;
                }
            }
        }
    }
    printf("metrics: %zu of %zu looks off by more than a pixel\n", off, compared);
    EXPECT_EQ(off, static_cast<size_t>(0));
    EXPECT_EQ(UI::Appearance::missing_tokens(), static_cast<size_t>(0));
}
