/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Vector.h>
#include <LibGfx/Path.h>

namespace Gfx {

enum class LineCap : u8 {
    Butt,
    Round,
    Square,
};

enum class LineJoin : u8 {
    Miter,
    Round,
    Bevel,
};

struct StrokeStyle {
    float width { 1 };
    LineCap cap { LineCap::Butt };
    LineJoin join { LineJoin::Miter };
    float miter_limit { 10 };
    // Lengths on and off, in turn; empty for a solid line.
    Vector<float> dashes {};
    float dash_phase { 0 };
};

// The outline of a stroke, to fill with the non-zero rule.
[[nodiscard]] Path stroked(Path const&, StrokeStyle const&, float tolerance = 0.025f);

}
