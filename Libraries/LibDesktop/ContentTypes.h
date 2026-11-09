/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Desktop.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

// What a file is, by the identifiers a Mac gives such things, and which application opens it: the registry of types in
// /System/Library/CoreServices/ContentTypes.json, the extensions applications say they open, and the choices someone's made
// (~/Library/Preferences/com.vulture.launchservices.json).

namespace Desktop {

struct ContentType {
    String identifier;
    String description;
    String icon;
    Vector<String> extensions;
    Vector<String> conforms_to;
};

[[nodiscard]] ContentType const* content_type_named(StringView identifier);
[[nodiscard]] ContentType const* content_type_for_extension(StringView extension);
// What's at a path, as far as its name and whether it's a folder can say: public.data if nothing can.
[[nodiscard]] StringView content_type_of_path(StringView path);
// As Files shows it: "Plain Text Document".
[[nodiscard]] StringView content_description_of_path(StringView path);
[[nodiscard]] bool content_type_conforms(StringView type, StringView to);

// The application that opens this if nobody says otherwise.
[[nodiscard]] Optional<AppInfo> default_application_for_path(StringView path);
// All that say they can, the default first: Open With's list.
[[nodiscard]] Vector<AppInfo> applications_for_path(StringView path);
// From now on, for everything of this type: "Change All…".
void set_default_application(StringView content_type, StringView application_identifier);

}
