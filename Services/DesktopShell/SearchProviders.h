/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Optional.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

// What Search can answer without looking for a file.

namespace DesktopShell {

// If this is arithmetic, what it comes to, as a person would write it.
[[nodiscard]] Optional<String> calculate(StringView expression);

// The panes of Settings whose names have a word that starts with this.
[[nodiscard]] Vector<StringView> settings_panes_matching(StringView text);

}
