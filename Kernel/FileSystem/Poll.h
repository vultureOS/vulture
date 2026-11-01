/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Locking/WaitQueue.h>

namespace Kernel {

// poll() waits here. Anything whose readiness changes (a pipe, a terminal)
// calls notify_pollers(); every poller then looks again at what it waits
// for. Simple, and plenty for a desktop's handful of pollers.
void notify_pollers();
// A change only some pollers care about: the generation moves (so nobody
// misses it), and the caller wakes those itself (ports wake the threads
// polling them, see Port).
void note_poll_change();
[[nodiscard]] u64 poll_generation();
[[nodiscard]] WaitQueue& poll_queue();

}
