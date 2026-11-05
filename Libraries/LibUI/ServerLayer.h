/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/OwnPtr.h>
#include <LibBase/Vector.h>
#include <LibGfx/Geometry.h>
#include <LibUI/Platform.h>

namespace UI {

class View;

// What a view has while it has a layer of its own in the window server. What's sent is remembered, so as to say only what's changed.
struct ServerLayer {
    ServerLayer(NonnullOwnPtr<LayerBackend> backend_, View* parent_)
        : backend(move(backend_))
        , parent(parent_)
    {
    }

    NonnullOwnPtr<LayerBackend> backend;
    View* parent { nullptr };
    // What the surface holds, in points, in the view's own bounds.
    Gfx::FloatRect surface_rect;
    Gfx::IntSize surface_pixels;
    // In the same terms: to be drawn again.
    Vector<Gfx::FloatRect> damage;
    bool present_all { false };

    Gfx::FloatRect sent_frame { 0, 0, -1, -1 };
    Gfx::FloatPoint sent_translation;
    Gfx::FloatPoint sent_bounds_origin;
    Gfx::FloatPoint sent_contents_origin;
    float sent_opacity { 1 };
    bool sent_clips { false };
    Gfx::FloatRect sent_clip { 0, 0, -1, -1 };
    size_t sent_order { ~static_cast<size_t>(0) };
};

}
