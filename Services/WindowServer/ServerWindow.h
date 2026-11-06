/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Layer.h"
#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibWindow/Protocol.h>

namespace WindowServer {

struct Client;
struct WindowShape;

struct MaterialRegion {
    Gfx::IntRect rect; // Points, from the window's top-left.
    Window::Material material { Window::Material::Sidebar };
    int radius { 0 }; // Points.
};

// A traffic light.
enum class WindowButton : u8 {
    None,
    Close,
    Minimize,
    Zoom,
};

struct ServerWindow {
    u32 id { 0 };
    Client* client { nullptr };
    String title;
    Gfx::IntRect frame; // Points.
    u32 flags { 0 };
    // Shown on screen now: follows from what is below, by update_visibility.
    bool visible { false };
    bool shown_by_client { false };
    bool minimized { false };
    int space { 0 };
    // Where it is drawn differs from its frame by this, in pixels, as spaces slide.
    int slide_x { 0 };
    int slide_y { 0 };
    // And while it is, it isn't seen outside this, in pixels; if there's more than one display it could end up on.
    Gfx::IntRect slide_clip;
    // A Space to itself.
    bool full_screen { false };
    Gfx::IntRect full_screen_restore;
    int full_screen_from { -1 };
    // Moved over to make room for Stage's strip.
    Gfx::IntRect stage_restore;
    // What it was before it was zoomed.
    Gfx::IntRect zoom_restore;
    // Tiled: how, and what it was before.
    int tile { -1 };
    Gfx::IntRect tile_restore;
    // Where clicks land, in points from its top-left; the whole of it, if empty.
    Gfx::IntRect input_region;
    bool active { false };
    int level { Window::LevelNormal };
    WindowShape const* shape { nullptr };
    Window::CursorShape cursor { Window::CursorShape::Arrow };
    // In points; 0 for the theme's.
    int title_bar_override { 0 };
    bool accepts_drop { false };

    // The client's memory, mapped here, and the two buffers in it.
    void* memory { nullptr };
    size_t memory_size { 0 };
    RefPtr<Gfx::Bitmap> buffers[2];
    int shown_buffer { 0 };
    bool has_presented { false };
    bool present_pending { false };
    // A new surface, not shown until it has something on it: the old one goes on being shown till then.
    void* waiting_memory { nullptr };
    size_t waiting_memory_size { 0 };
    RefPtr<Gfx::Bitmap> waiting_buffers[2];
    // Being resized by hand. The frame stays as it is until the program has something of the new size to show, and then they both
    // change in the same frame, so that its edges are never seen ahead of its contents. Unless it's too long about it.
    bool resize_waiting { false };
    Gfx::IntRect resize_to;
    u64 resize_asked_at { 0 };
    bool resize_again { false };
    Gfx::IntRect resize_next;
    bool resize_gave_up { false };

    // Which squares of the shown surface are wholly opaque, left to right and top to bottom: nothing need be drawn under them.
    static constexpr int opaque_cell_size = 32;
    Vector<u8> opaque_cells;
    int opaque_cell_columns { 0 };
    int opaque_cell_rows { 0 };

    Vector<MaterialRegion> materials;

    // What's over its surface: all of them, and those directly on the window, bottom to top.
    Vector<NonnullOwnPtr<Layer>> layers;
    Vector<Layer*> top_layers;
    u32 next_layer_id { 1 };
    bool layers_present_pending { false };
    u32 layers_present_serial { 0 };

    [[nodiscard]] Layer* layer_with_id(u32 layer_id)
    {
        for (auto& layer : layers) {
            if (layer->id == layer_id)
                return layer.ptr();
        }
        return nullptr;
    }

    // The title bar, drawn over the client's pixels.
    RefPtr<Gfx::Bitmap> chrome;
    bool chrome_is_stale { true };
    bool chrome_is_opaque { false };
    bool lights_hovered { false };
    WindowButton pressed_button { WindowButton::None };

    [[nodiscard]] bool is_bare() const { return flags & Window::WindowBare; }
    // Nothing around it: no corners and no shadow.
    [[nodiscard]] bool is_square() const { return is_bare() || full_screen; }
    [[nodiscard]] bool is_titled() const { return flags & Window::WindowTitled; }
    [[nodiscard]] bool is_resizable() const { return flags & Window::WindowResizable; }
    [[nodiscard]] bool is_popup() const { return flags & Window::WindowPopup; }
    [[nodiscard]] bool is_unified() const { return flags & Window::WindowUnifiedTitle; }
    [[nodiscard]] bool can_become_key() const { return !is_popup() && (!is_bare() || (flags & Window::WindowCanBecomeKey)); }
    [[nodiscard]] bool is_on_every_space() const { return (flags & Window::WindowSticky) || level != Window::LevelNormal; }
    // On the Dock's and Overview's minds: a window of an application's own.
    [[nodiscard]] bool is_document_window() const { return is_titled() && !is_popup(); }
    [[nodiscard]] Gfx::Bitmap const* shown_surface() const { return has_presented ? buffers[shown_buffer].ptr() : nullptr; }
};

}
