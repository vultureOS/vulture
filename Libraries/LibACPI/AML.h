/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Optional.h>
#include <LibBase/Span.h>
#include <LibBase/Types.h>
#include <LibBase/Vector.h>

// As much of ACPI's machine language as this system needs, which is to read what the firmware has
// written down and not to run any of it. A table's bytecode is a tree of named scopes, and its length
// fields mean all of it can be walked without understanding what's in the methods:
//
//   \_S5_, a package saying what to write to the power management registers to power off.
//   _PRT, under a PCI host bridge, a package of packages saying which interrupt each slot has, if
//   it was written down as a plain package and not computed by a method.
//
// This is enough for anything that gives devices message interrupts. A machine whose _PRT is computed
// has no answer here: devices without message interrupts then have no interrupts.

namespace ACPI {

struct SleepType {
    u8 a;
    u8 b;
};

struct InterruptRoute {
    u32 slot;    // The device number on bus 0.
    u8 pin;      // 0 is INTA.
    u32 line;    // The global system interrupt, if `direct`.
    bool direct; // False: it went by way of a link device, which this doesn't follow.
};

// What \_S5_ says.
[[nodiscard]] Optional<SleepType> find_sleep_type_s5(ReadonlyBytes table);

// Every route in every plain _PRT in the table.
void find_interrupt_routes(ReadonlyBytes table, Vector<InterruptRoute>& routes);

}
