/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Assertions.h>
#include <LibUI/Platform.h>

namespace UI {

static Platform* s_platform;

Platform& Platform::the()
{
    VERIFY(s_platform);
    return *s_platform;
}

void Platform::set(Platform* platform)
{
    s_platform = platform;
}

ErrorOr<NonnullOwnPtr<LayerBackend>> WindowBackend::create_layer(LayerBackend const*)
{
    return Error::from_string_literal("There are no layers here");
}

}
