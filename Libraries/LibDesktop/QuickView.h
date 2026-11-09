/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/RefPtr.h>
#include <LibBase/String.h>
#include <LibGfx/Bitmap.h>

// A picture of what's in a file, made somewhere that can't do any harm: the file is opened here and given to a process with
// a sandbox of its own, and whatever's in it can at worst bring that down.

namespace Desktop {

// Fitted in this many pixels. Comes back on the event loop, with nothing if it couldn't be done.
void request_preview(StringView path, int most_width, int most_height, Function<void(RefPtr<Gfx::Bitmap>)> then);

// As request_preview, kept for next time under ~/Library/Caches, for as long as the file's the same.
void request_thumbnail(StringView path, int pixels, Function<void(RefPtr<Gfx::Bitmap>)> then);

// Whether there's any point asking: whether something's said it pictures this type.
[[nodiscard]] bool can_preview(StringView path);

}
