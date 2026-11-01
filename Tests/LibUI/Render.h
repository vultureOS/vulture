/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Catalog.h"
#include <LibBase/HashMap.h>
#include <LibBase/String.h>
#include <LibGfx/Bitmap.h>

struct Look {
    StringView name;
    StringView appearance;
    int scale;
    // How Tests/UI/Measured/controls.json files it.
    StringView key;
};

[[nodiscard]] Span<Look const> all_looks();

// What AppKit reported of a control.
struct MeasuredBox {
    int x { 0 }, y { 0 }, width { 0 }, height { 0 };
};

struct MeasuredControl {
    float frame_width { 0 }, frame_height { 0 };
    float intrinsic_width { 0 }, intrinsic_height { 0 };
    float inset_top { 0 }, inset_left { 0 }, inset_bottom { 0 }, inset_right { 0 };
    MeasuredBox box[4];
    MeasuredBox soft_box[4];
};

// Tests/UI/Measured/controls.json, by name.
[[nodiscard]] HashMap<String, MeasuredControl> read_measured_controls();

struct Rendered {
    Gfx::IntRect box;
    Gfx::IntRect soft_box;
    Vector<Gfx::Pixel> down;
    Vector<Gfx::Pixel> across;
    RefPtr<Gfx::Bitmap> picture;
    Gfx::IntSize intrinsic;
};

// Draws it as measure.m did: on nothing, with the frame AppKit gave it.
[[nodiscard]] Rendered render_entry(CatalogEntry const&, Look const&, HashMap<String, MeasuredControl> const&);
