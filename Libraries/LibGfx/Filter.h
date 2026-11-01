/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibGfx/Bitmap.h>

// Whole-area operations, for materials and shadows.

namespace Gfx {

// A Gaussian blur of a part of the bitmap (standard deviation `sigma`, in
// pixels), by three box blurs as SVG specifies them; what is beyond the edges
// repeats the edge. EINVAL-free: a sigma too small to show does nothing.
ErrorOr<void> gaussian_blur(Bitmap&, IntRect region, float sigma);
ErrorOr<void> gaussian_blur(AlphaMask&, float sigma);

// As CSS's saturate(): 0 grey, 1 unchanged, above 1 more vivid.
void adjust_saturation(Bitmap&, IntRect region, float saturation);

}
