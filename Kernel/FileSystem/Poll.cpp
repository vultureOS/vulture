/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/FileSystem/Poll.h>

namespace Kernel {

static Atomic<u64> s_generation { 0 };

WaitQueue& poll_queue()
{
    static WaitQueue queue;
    return queue;
}

u64 poll_generation()
{
    return s_generation.load(MemoryOrder::Acquire);
}

void notify_pollers()
{
    s_generation.fetch_add(1, MemoryOrder::Release);
    poll_queue().wake_all();
}

}
