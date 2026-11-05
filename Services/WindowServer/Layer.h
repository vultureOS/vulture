/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibAnimation/Curve.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibWindow/Protocol.h>

namespace WindowServer {

// What a layer has that moves. It has been told one set of these, and while an animation's running, shows another.
struct LayerState {
    Gfx::FloatRect frame; // Points, among what it's in.
    Gfx::FloatPoint translation;
    Gfx::FloatPoint scale { 1, 1 }; // About its middle.
    float rotation { 0 };           // About its middle.
    float opacity { 1 };
    float corner_radius { 0 };
    Gfx::FloatPoint bounds_origin; // What's at its top left, for what's in it, as something scrolled.
};

// A value on its way, run here: the program that started it may be doing something else by now, or nothing at all.
struct LayerAnimation {
    Window::LayerProperty property { Window::LayerProperty::Opacity };
    float from[4] {};
    float to[4] {};
    Animation::Timing timing;
    double started { 0 };
    bool repeats { false };
    int steps { 0 };
    u32 tag { 0 };
};

// What a frame showed, for a test to hold against what should have been.
struct LayerLogEntry {
    double seconds { 0 }; // Since the animation started.
    float values[4] {};
};

// Something over a window's own surface, moved and faded here without asking the program that owns it. Each window has a tree
// of them, drawn in order after the window itself.
struct Layer {
    ~Layer()
    {
        release_surface();
    }

    void release_surface();
    [[nodiscard]] Gfx::Bitmap const* shown_surface() const { return has_presented ? buffers[shown_buffer].ptr() : nullptr; }

    u32 id { 0 };
    Layer* parent { nullptr };
    Vector<Layer*> children; // Bottom to top.

    LayerState model;
    LayerState shown;
    Gfx::FloatPoint contents_origin; // Points: where the surface's top left goes, among what it holds.
    Gfx::Color background { 0, 0, 0, 0 };
    bool hidden { false };
    bool clips { false };
    int material { -1 };
    float blur { 0 }; // Points: of what it shows.
    // Where what's around it cuts it off: points, among what it's in.
    Gfx::FloatRect clip_rect;
    bool has_clip_rect { false };

    void* memory { nullptr };
    size_t memory_size { 0 };
    RefPtr<Gfx::Bitmap> buffers[2];
    int shown_buffer { 0 };
    bool has_presented { false };

    Vector<LayerAnimation> animations;
    bool logged { false };
    Vector<LayerLogEntry> log;
};

}
