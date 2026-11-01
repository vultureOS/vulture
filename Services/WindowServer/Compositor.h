/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Screen.h"
#include "ServerWindow.h"
#include "Theme.h"
#include "WorkerPool.h"
#include <LibGfx/Bitmap.h>

namespace WindowServer {

// A blurred shape, stored small: corners as they are, edges one row or column
// repeated. For window shadows and outlines.
struct NineSlice {
    RefPtr<Gfx::AlphaMask> mask;
    int margin { 0 }; // How far it reaches outside the frame.
    int inset { 0 };  // From the mask's edge to where it stops changing.
};

// A window's outline, and all that follows from it, at one corner radius.
struct WindowShape {
    int radius { 0 }; // Points. The theme's, if `follows_theme`.
    bool follows_theme { false };
    // The top-left corner's coverage; the others are it mirrored.
    RefPtr<Gfx::AlphaMask> corner;
    int corner_extent { 0 };
    NineSlice outline;
    NineSlice shadow_active;
    NineSlice shadow_inactive;
    NineSlice shadow_popup;
};

// Draws the screen, from the bottom up, wherever something changed: only there.
// The screen is a grid of tiles, each dirty or not; each layer draws over the
// dirty tiles on all the worker threads at once. A material needs what is below it
// finished first, so it splits the job; and its blur reaches past what changed, so
// the dirty area first grows by that much around every material it touches.
class Compositor {
public:
    Compositor(Screen&, Theme const&, Vector<ServerWindow*> const& windows_bottom_to_top);

    [[nodiscard]] ErrorOr<void> initialize();
    // The theme object changed: everything drawn from it is made anew, and shown.
    [[nodiscard]] ErrorOr<void> theme_changed();

    // Lives as long as this does (and changes with the theme).
    [[nodiscard]] WindowShape const* default_shape() const { return m_shapes[0].ptr(); }
    [[nodiscard]] ErrorOr<WindowShape const*> shape_with_radius(int points);

    void invalidate(Gfx::IntRect pixels);
    void invalidate_all();
    // What a window covers, shadow included.
    void invalidate_window(ServerWindow const&);
    [[nodiscard]] Gfx::IntRect window_bounds(ServerWindow const&) const;
    [[nodiscard]] Gfx::IntRect device_frame(ServerWindow const& window) const;

    [[nodiscard]] bool has_damage() const { return m_any_dirty; }
    void render();

    [[nodiscard]] u64 frames() const { return m_frames; }
    [[nodiscard]] u64 compose_microseconds() const { return m_compose_microseconds; }
    [[nodiscard]] u64 flush_microseconds() const { return m_flush_microseconds; }
    [[nodiscard]] u64 pixels() const { return m_pixels; }

private:
    static constexpr int tile_size = 32;
    // The height of a piece of work.
    static constexpr int band_height = 64;

    enum class LayerKind : u8 {
        Wallpaper,
        Shadow,
        Body,
    };

    struct Layer {
        LayerKind kind;
        ServerWindow const* window;
    };

    struct LayerJob {
        Layer const* layers;
        size_t layer_count;
        Vector<Gfx::IntRect> const* pieces;
        Compositor* compositor;
    };

    ErrorOr<void> render_wallpaper();
    ErrorOr<NineSlice> make_shadow(WindowShape const&, ShadowStyle const&);
    ErrorOr<NineSlice> make_outline(WindowShape const&);
    ErrorOr<void> make_shape(WindowShape&);
    [[nodiscard]] ShadowStyle const& shadow_style_of(ServerWindow const&) const;
    [[nodiscard]] NineSlice const& shadow_slice_of(ServerWindow const&) const;

    void grow_damage_for_materials();
    [[nodiscard]] Vector<Gfx::IntRect> dirty_rects() const;
    [[nodiscard]] Vector<Gfx::IntRect> pieces_of(Vector<Gfx::IntRect> const&) const;
    void draw_layers(Layer const*, size_t count, Vector<Gfx::IntRect> const& pieces);

    void draw_wallpaper(Gfx::IntRect clip);
    void draw_shadow(ServerWindow const&, Gfx::IntRect clip);
    void draw_body(ServerWindow const&, Gfx::IntRect clip);
    void draw_nine_slice(NineSlice const&, Gfx::IntRect target, Gfx::Color, Gfx::IntRect clip, Gfx::IntRect hole, int hole_corner);
    void apply_material(ServerWindow const&, Gfx::IntRect region, MaterialStyle const&, Gfx::IntRect area);

    // Blends a row of pixels into the screen through the window's rounded
    // corners. x is the first column to draw, in device pixels from the frame's
    // left; y the row, from its top.
    void blend_row_in_window(Gfx::Pixel* destination, Gfx::Pixel const* source, int x, int y, int count, Gfx::IntSize frame_size, WindowShape const*) const;

    Screen& m_screen;
    Theme const& m_theme;
    Vector<ServerWindow*> const& m_windows;
    WorkerPool m_workers;
    RefPtr<Gfx::Bitmap> m_wallpaper;
    Vector<NonnullOwnPtr<WindowShape>> m_shapes;

    int m_tiles_across { 0 };
    int m_tiles_down { 0 };
    Vector<u8> m_dirty;
    bool m_any_dirty { false };

    u64 m_frames { 0 };
    u64 m_compose_microseconds { 0 };
    u64 m_flush_microseconds { 0 };
    u64 m_pixels { 0 };
};

}
