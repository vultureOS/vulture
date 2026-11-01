/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/Vector.h>
#include <LibUI/View.h>

// Every control as Meta/Calibration/measure.m set it up in AppKit, by the same names:
// what the numbers in Tests/UI/Measured are held against.

struct CatalogEntry {
    StringView name;
    NonnullOwnPtr<UI::View> (*make)();
};

[[nodiscard]] Vector<CatalogEntry> control_catalog();
