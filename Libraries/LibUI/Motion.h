/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibAnimation/Animator.h>
#include <LibAnimation/Curve.h>

namespace UI {

// The program's one animator, on a timer that runs only while something moves.
[[nodiscard]] Animation::Animator& motion();

// The standard duration, from the theme, and whether to bother: tests turn motion
// off, so that what they look at does not depend on when they look.
[[nodiscard]] Animation::Timing standard_timing();
void set_motion_enabled(bool);
[[nodiscard]] bool motion_enabled();
// Drives `apply` from 0 to 1 by this timing: at once, when motion is off.
Animation::Animator::Id animate(Animation::Timing const&, Function<void(float)> apply, Function<void()> done = nullptr);

}
