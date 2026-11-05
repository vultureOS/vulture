/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibGfx/Bitmap.h>
#include <LibWindow/Protocol.h>

namespace WindowServer {

// The display's cursor is this size in pixels: at 2x, it holds 32 points.
constexpr int VULTURE_CURSOR_IMAGE_SIZE = 64;

struct CursorImage {
    NonnullRefPtr<Gfx::Bitmap> image;
    Gfx::IntPoint hot_spot;
};

// How far round the one that goes round is goes in this many steps.
constexpr int wait_cursor_steps = 24;

[[nodiscard]] CursorImage make_cursor(Window::CursorShape, int scale, int turn = 0);

}
