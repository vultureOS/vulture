/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Services/QuickView/Plugin.h>
#include <string.h>

// For what's called something.crashtest: Quick View has to carry on as if nothing had happened.

extern "C" int vulture_quickview_claims(char const*, char const* extension)
{
    return strcmp(extension, "crashtest") == 0;
}

extern "C" int vulture_quickview_render(int, char const*, unsigned, unsigned, VultureQuickViewPicture*)
{
    *reinterpret_cast<int volatile*>(16) = 1;
    return 0;
}
