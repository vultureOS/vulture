/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>

// What makes two names one name. All of this was measured on a Mac's APFS, and
// the area `names` of Tests/Userland/PosixSystem.h goes on comparing:
//
// - A name has to be UTF-8, or it can't be made (EILSEQ) and isn't there to be found (ENOENT).
// - It may be 255 UTF-16 units long, as it's given, however many bytes those are.
// - Canonically equivalent names are one name, on every volume.
// - On a case-insensitive one, so are those that Unicode's full case folding makes equal, with
//   no special treatment for Turkish: "straße" is "STRASSE", and "ı" isn't "I".
// - Nothing else is: not joiners, nor variation selectors, nor compatibility equivalents.
//
// The name stays as it was given. This is only what it's compared by, and the tables behind it
// are part of the format: a volume made with these will go on being looked up with these.

namespace VultureFS {

[[nodiscard]] bool is_valid_name_encoding(StringView);
// How long it is by a Mac's way of counting. Only for what's valid.
[[nodiscard]] size_t name_length_in_utf16_units(StringView);

// Its canonical decomposition, with its case folded if it's to be.
ErrorOr<void> fold_name(StringView, bool case_insensitive, Vector<u8>& folded);

}
