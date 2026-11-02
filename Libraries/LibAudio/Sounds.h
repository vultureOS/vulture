/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibAudio/Sound.h>
#include <LibBase/StringView.h>

// The system's sounds. All are computed, from sine waves and a little noise, so that there
// is nothing to license and nothing recorded to carry around.

namespace Audio {

// "notification", "alert", "trash", "login" and "volume".
[[nodiscard]] Span<StringView const> named_sounds();
// Null if there is no sound by that name.
[[nodiscard]] RefPtr<Sound const> make_named_sound(StringView);
// A plain tone, fading in and out so as not to click.
[[nodiscard]] NonnullRefPtr<Sound const> make_tone(double frequency, double seconds, float amplitude);

}
