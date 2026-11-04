/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

namespace Locale::Tables {

// Each entry of these has a code point or more, then what they come to: a collation element, packed as primary:16 secondary:10 tertiary:5 variable:1; or where such
// elements are, in collation_elements, shifted left by 8, with how many of them in the bottom byte.
extern u32 const single_elements[];
extern size_t const single_elements_count;
extern u32 const expansions[];
extern size_t const expansions_count;
extern u32 const contraction_entries[];
extern size_t const contraction_entries_count;
extern u32 const collation_elements[];
extern size_t const collation_elements_count;

struct TailoringRules {
    char const* language;
    char const* rules;
};

extern TailoringRules const tailoring_rules[];
extern size_t const tailoring_rules_count;

}
