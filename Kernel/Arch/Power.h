/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Format.h>

namespace DeviceTree {
class FlattenedDeviceTree;
}

namespace Kernel::Arch {

// Finds the firmware interface that turns the machine (and processors) on and off.
ErrorOr<void> initialize_power_management(DeviceTree::FlattenedDeviceTree const&);
void describe_power_management(FormatSink&);

// Without power management, both of these halt instead.
[[noreturn]] void power_off();
[[noreturn]] void reboot();

}
