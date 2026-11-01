/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibGfx/Bitmap.h>
#include <LibWindow/Protocol.h>

namespace WindowServer {

struct CursorImage {
    NonnullRefPtr<Gfx::Bitmap> image;
    Gfx::IntPoint hot_spot;
};

[[nodiscard]] CursorImage make_cursor(Window::CursorShape, int scale);

}
