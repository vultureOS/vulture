/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// The kernel's built-in 8x16 bitmap font for the boot and panic consoles.
// It covers printable ASCII; everything else shows as '?'.

namespace Kernel::Font {

constexpr u32 glyph_width = 8;
constexpr u32 glyph_height = 16;

// glyph_height rows, one byte each; the most significant bit is the leftmost pixel.
[[nodiscard]] u8 const* glyph(char);

}
