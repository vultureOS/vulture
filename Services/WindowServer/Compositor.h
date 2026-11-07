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
#include <LibBase/Optional.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibText/Font.h>

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
    // The displays are somewhere else now.
    [[nodiscard]] ErrorOr<void> screen_changed();
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

    // A surface has been replaced, or part of the one that's shown is new: what's opaque of it is worked out here.
    void surface_replaced(ServerWindow&);
    void surface_changed(ServerWindow&, Gfx::IntRect damage);
    // All that this layer may draw on: it has changed, or is about to.
    void invalidate_layer(ServerWindow const&, Layer const&);
    [[nodiscard]] Gfx::IntRect device_rect_of_layer_surface(ServerWindow const&, Layer const&, Gfx::IntRect pixels) const;

    // What is drawn in place of windows, while they move through a shape they cannot
    // have: between the normal windows and those above. All on one thread, by painter.
    struct Overlay {
        enum class Kind : u8 {
            // Scaled into a rectangle, with rounded corners.
            Thumbnail,
            // Narrowing toward a point below, as a window going into the Dock.
            Genie,
            // A rounded rectangle, of a color.
            Tile,
            // The system's mark.
            Logo,
        };
        Kind kind { Kind::Thumbnail };
        RefPtr<Gfx::Bitmap> picture;
        Gfx::FloatRect rect;
        // For a genie: from 0 (where it was) to 1 (gone).
        float progress { 0 };
        Gfx::FloatPoint target;
        Gfx::Color color;
        float radius { 0 };
        String label;
        // The size of a caption, rather than of a title.
        bool small_label { false };
        bool shadow { false };
    };
    void set_overlays(Vector<Overlay>);
    // The wallpaper of this part of the desktop, in points, this small, in points, with its corners rounded.
    [[nodiscard]] ErrorOr<NonnullRefPtr<Gfx::Bitmap>> miniature_of_wallpaper(Gfx::IntRect points, Gfx::IntSize size, float radius) const;
    // This part of it, in pixels, likewise.
    [[nodiscard]] ErrorOr<NonnullRefPtr<Gfx::Bitmap>> miniature_of(Gfx::Bitmap const&, Gfx::IntRect part, Gfx::IntSize size, float radius) const;
    void set_dim(Gfx::Color);
    // Where a window being dragged would go if it were let go now, in points: nothing, if it's empty.
    void set_tile_preview(Gfx::FloatRect);
    [[nodiscard]] Gfx::Color dim() const { return m_dim; }
    void set_overlay_font(Text::Font* font) { m_overlay_font = font; }
    void set_overlay_small_font(Text::Font* font) { m_overlay_small_font = font; }
    // A picture of a window as it now is, title bar and all.
    [[nodiscard]] ErrorOr<NonnullRefPtr<Gfx::Bitmap>> snapshot(ServerWindow const&) const;
    // As a screenshot of a window has it: with its outline, and its shadow round it, on nothing.
    [[nodiscard]] ErrorOr<NonnullRefPtr<Gfx::Bitmap>> snapshot_with_shadow(ServerWindow const&) const;
    // Hides the windows below the Dock's level, for Overview.
    void set_hides_normal_windows(bool hides) { m_hides_normal_windows = hides; }
    // A wallpaper by its colors.
    void set_wallpaper_colors(Vector<Gfx::Color> colors);

    [[nodiscard]] u64 frames() const { return m_frames; }
    [[nodiscard]] u64 compose_microseconds() const { return m_compose_microseconds; }
    [[nodiscard]] u64 flush_microseconds() const { return m_flush_microseconds; }
    [[nodiscard]] u64 pixels() const { return m_pixels; }
    [[nodiscard]] u64 material_microseconds() const { return m_material_microseconds; }

private:
    static constexpr int tile_size = 32;
    // The height of a piece of work.
    static constexpr int band_height = 64;

    enum class StepKind : u8 {
        Wallpaper,
        Shadow,
        Body,
        Layer,
    };

    // One thing to draw over all of what's dirty. `index` is the window's place, from the bottom.
    struct Step {
        StepKind kind;
        ServerWindow const* window;
        int index;
        // What of the layers, for that kind.
        size_t draw { 0 };
    };

    // A layer comes down to these: what to put where, worked out for each frame.
    struct LayerDraw {
        enum class Kind : u8 {
            Image,
            Color,
            Material,
        };
        Kind kind { Kind::Color };
        int window_index { -1 };
        // The window's shape, and where its corners are, for a layer that goes as far as those.
        WindowShape const* shape { nullptr };
        Gfx::IntRect window_frame;
        Gfx::Bitmap const* image { nullptr };
        // A picture made for this frame, of something that can't be drawn straight on to the screen.
        RefPtr<Gfx::Bitmap> picture;
        Gfx::Pixel color { 0 };
        MaterialStyle const* material { nullptr };
        // Whole pixels, one to one: most of them. Otherwise it goes by the map.
        bool integral { true };
        Gfx::IntRect rect;
        Gfx::AffineTransform device_to_image;
        Gfx::IntRect bounds; // Where it can draw at all.
        u32 opacity { 255 };
        // Device pixels, of `rounded`, which is the layer's whole frame.
        float radius { 0 };
        Gfx::IntRect rounded;
    };

    // Part of what's dirty, and the lowest window that shows in it: nothing under that is drawn. -1 if the wallpaper does.
    struct Piece {
        Gfx::IntRect rect;
        int lowest;
        // The steps done so far: a piece is only taken as far as something needs it to be.
        size_t next_step { 0 };
    };

    struct StepJob {
        Vector<Step> const* steps;
        size_t end;
        Vector<Piece*> const* pieces;
        Compositor* compositor;
    };

    // Some of a material on the screen. What it's to read, and the small picture it's read into.
    struct MaterialJob {
        Compositor* compositor;
        ServerWindow const* window;
        MaterialStyle const* style;
        Gfx::IntRect area;
        Gfx::IntRect region;
        int radius;
        Gfx::IntRect source;
        int grid_left;
        int grid_top;
        int small_width;
        int small_height;
        int reduction;
        float sigma;
        Gfx::Bitmap* small;
        Gfx::Bitmap* spare;
    };

    struct MaterialBand {
        MaterialJob const* job;
        int first;
        int end;
    };

    // Kept from one frame to the next: making and letting go of this much memory every frame is what used to take the time.
    struct MaterialMemory {
        RefPtr<Gfx::Bitmap> small;
        RefPtr<Gfx::Bitmap> spare;
    };

    // A material that can be seen, and how much of it.
    struct ShownMaterial {
        size_t window_index;
        size_t material_index;
        Gfx::IntRect region;
        Gfx::IntRect shown;
        int reach;
        // Behind a layer, if it isn't one of the window's own. Then the window's own pixels are under it.
        Optional<size_t> draw;
    };

    ErrorOr<void> render_wallpaper();
    void draw_overlays(Vector<Gfx::IntRect> const& rects);
    void draw_tile_preview(Vector<Gfx::IntRect> const& rects);
    ErrorOr<NineSlice> make_shadow(WindowShape const&, ShadowStyle const&);
    ErrorOr<NineSlice> make_outline(WindowShape const&);
    ErrorOr<void> make_shape(WindowShape&);
    [[nodiscard]] ShadowStyle const& shadow_style_of(ServerWindow const&) const;
    [[nodiscard]] NineSlice const& shadow_slice_of(ServerWindow const&) const;

    void collect_layer(Layer const&, Gfx::AffineTransform const& parent, Gfx::IntRect clip, int window_index, ServerWindow const&, Gfx::IntPoint frame_origin, Vector<LayerDraw>&);
    void collect_layer_contents(Layer const&, Gfx::AffineTransform const& to_device, Gfx::IntRect clip, u32 opacity, float radius, Gfx::IntRect rounded, int window_index, ServerWindow const&, Gfx::IntPoint frame_origin, Vector<LayerDraw>&);
    void draw_layer_piece(LayerDraw const&, Gfx::Bitmap& target, Gfx::IntRect clip) const;
    void prepare_layer_draws();
    [[nodiscard]] Gfx::AffineTransform layer_to_device(ServerWindow const&, Layer const&) const;
    [[nodiscard]] Gfx::IntRect layer_bounds(ServerWindow const&, Layer const&, Gfx::AffineTransform const& parent) const;
    [[nodiscard]] bool is_drawn(ServerWindow const&) const;
    [[nodiscard]] bool covers_tile(ServerWindow const&, Gfx::IntRect frame, Gfx::IntRect tile) const;
    [[nodiscard]] Gfx::IntRect tile_rect(int across, int down) const;
    // For each tile, the lowest window that shows there; and so which materials can be seen, and what they have to read.
    void find_what_shows();
    void grow_damage_for_materials();
    // Whether that made any part dirty that was not.
    bool invalidate_reporting(Gfx::IntRect);
    [[nodiscard]] Vector<Gfx::IntRect> dirty_rects() const;
    [[nodiscard]] Vector<Piece> pieces_of(Vector<Gfx::IntRect> const&) const;
    void draw_steps(Vector<Step> const&, size_t end, Vector<Piece*> const& pieces);
    void draw_materials(Vector<MaterialJob>&);

    void draw_wallpaper(Gfx::IntRect clip);
    void draw_shadow(ServerWindow const&, Gfx::IntRect clip);
    // `covers_it`: every pixel it has here is opaque, and it's on none of its corners.
    void draw_body(ServerWindow const&, Gfx::IntRect clip, bool covers_it);
    // Into the framebuffer, or into this picture instead.
    void draw_nine_slice(NineSlice const&, Gfx::IntRect target, Gfx::Color, Gfx::IntRect clip, Gfx::IntRect hole, int hole_corner, Gfx::Bitmap* into = nullptr);
    // What shows through a material here: reading the screen into a small picture, blurring that, and bringing it back up.
    void read_material_rows(MaterialJob const&, int first, int end);
    void blur_material(MaterialJob const&);
    void place_material_rows(MaterialJob const&, int first, int end);
    void place_material_row(MaterialJob const&, int y, Gfx::Pixel const* source);

    // Blends a row of pixels into the screen through the window's rounded
    // corners. x is the first column to draw, in device pixels from the frame's
    // left; y the row, from its top.
    void blend_row_in_window(Gfx::Pixel* destination, Gfx::Pixel const* source, int x, int y, int count, Gfx::IntSize frame_size, WindowShape const*) const;

    Screen& m_screen;
    Theme const& m_theme;
    Vector<ServerWindow*> const& m_windows;
    WorkerPool m_workers;
    RefPtr<Gfx::Bitmap> m_wallpaper;
    Vector<Gfx::Color> m_wallpaper_colors;
    Vector<Overlay> m_overlays;
    Gfx::Color m_dim { 0, 0, 0, 0 };
    Gfx::FloatRect m_tile_preview;
    bool m_hides_normal_windows { false };
    Text::Font* m_overlay_font { nullptr };
    Text::Font* m_overlay_small_font { nullptr };
    Vector<NonnullOwnPtr<WindowShape>> m_shapes;

    int m_tiles_across { 0 };
    int m_tiles_down { 0 };
    Vector<u8> m_dirty;
    bool m_any_dirty { false };
    Vector<i16> m_lowest_shown;
    Vector<ShownMaterial> m_shown_materials;
    Vector<MaterialMemory> m_material_memory;
    Vector<LayerDraw> m_layer_draws;

    u64 m_frames { 0 };
    u64 m_compose_microseconds { 0 };
    u64 m_flush_microseconds { 0 };
    u64 m_pixels { 0 };
    u64 m_material_microseconds { 0 };
};

}
