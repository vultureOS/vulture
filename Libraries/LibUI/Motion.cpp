/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Appearance.h>
#include <LibUI/Motion.h>
#include <LibUI/Platform.h>

namespace UI {

namespace {

bool s_enabled { true };
int s_timer { -1 };

Animation::Animator& the_animator()
{
    static Animation::Animator* animator = nullptr;
    if (!animator) {
        animator = new Animation::Animator;
        animator->on_became_active = [] {
            if (s_timer >= 0)
                return;
            s_timer = Platform::the().add_timer(16, true, [] {
                the_animator().tick(Platform::the().now());
                if (!the_animator().is_active() && s_timer >= 0) {
                    Platform::the().cancel_timer(s_timer);
                    s_timer = -1;
                }
            });
        };
    }
    return *animator;
}

}

Animation::Animator& motion()
{
    return the_animator();
}

Animation::Timing standard_timing()
{
    return Animation::Timing::ease_in_out(Appearance::the().metric("animation.standard"_sv));
}

void set_motion_enabled(bool enabled)
{
    s_enabled = enabled;
}

bool motion_enabled()
{
    return s_enabled;
}

Animation::Animator::Id animate(Animation::Timing const& timing, Function<void(float)> apply, Function<void()> done)
{
    if (!s_enabled) {
        apply(1);
        if (done)
            done();
        return 0;
    }
    return the_animator().start(timing, move(apply), move(done));
}

}
