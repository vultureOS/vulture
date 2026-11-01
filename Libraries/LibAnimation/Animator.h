/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibAnimation/Curve.h>
#include <LibBase/Function.h>
#include <LibBase/Vector.h>

namespace Animation {

// What runs animations: told the time, it calls each with its progress. It has no
// clock of its own, so tests can run it by hand and programs off their frames.
class Animator {
public:
    using Id = u32;

    // apply(progress) every tick, and with exactly 1 at the end; then done(), unless
    // it was cancelled.
    Id start(Timing const&, Function<void(float)> apply, Function<void()> done = nullptr);
    // No more calls to it. Nothing happens for an id that has finished.
    void cancel(Id);
    [[nodiscard]] bool is_running(Id) const;

    // Runs everything up to `seconds` (of any clock that only goes forwards). The
    // callbacks may start or cancel others, which first run on the next tick.
    void tick(double seconds);

    [[nodiscard]] bool is_active() const { return !m_running.is_empty(); }
    Function<void()> on_became_active;

private:
    struct Running {
        Id id;
        Timing timing;
        Function<void(float)> apply;
        Function<void()> done;
        double started { -1 };
        bool cancelled { false };
    };

    Vector<Running> m_running;
    Id m_next_id { 1 };
    bool m_ticking { false };
};

}
