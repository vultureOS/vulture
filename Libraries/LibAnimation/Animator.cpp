/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibAnimation/Animator.h>

namespace Animation {

Animator::Id Animator::start(Timing const& timing, Function<void(float)> apply, Function<void()> done)
{
    bool was_active = is_active();
    Id id = m_next_id++;
    m_running.append(Running { id, timing, move(apply), move(done) });
    if (!was_active && on_became_active)
        on_became_active();
    return id;
}

void Animator::cancel(Id id)
{
    for (auto& running : m_running) {
        if (running.id == id)
            running.cancelled = true;
    }
    if (!m_ticking)
        m_running.remove_all_matching([](auto const& running) { return running.cancelled; });
}

bool Animator::is_running(Id id) const
{
    for (auto const& running : m_running) {
        if (running.id == id && !running.cancelled)
            return true;
    }
    return false;
}

void Animator::tick(double seconds)
{
    m_ticking = true;
    // By index, and only those here now: callbacks may append (and so move) them.
    size_t count = m_running.size();
    Vector<Function<void()>> finished;
    for (size_t i = 0; i < count; ++i) {
        if (m_running[i].cancelled)
            continue;
        if (m_running[i].started < 0)
            m_running[i].started = seconds;
        auto elapsed = static_cast<float>(seconds - m_running[i].started);
        bool ended = elapsed >= m_running[i].timing.duration;
        float progress = ended ? 1 : m_running[i].timing.value_at(elapsed);
        // The callback moves with the vector if it appends, so it runs from here.
        auto apply = move(m_running[i].apply);
        apply(progress);
        if (ended) {
            m_running[i].cancelled = true;
            if (m_running[i].done)
                finished.append(move(m_running[i].done));
        } else {
            m_running[i].apply = move(apply);
        }
    }
    m_running.remove_all_matching([](auto const& running) { return running.cancelled; });
    m_ticking = false;
    for (auto& done : finished)
        done();
}

}
