/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Compositor.h"
#include <LibBase/Math.h>
#include <LibGfx/Blend.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Rasterizer.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace WindowServer {

namespace Math = Base::Math;
using namespace Gfx;

namespace {

u64 now_microseconds()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<u64>(now.tv_sec) * 1'000'000 + static_cast<u64>(now.tv_nsec) / 1000;
}

// The worker threads, without taking every processor from everyone else.
int worker_count()
{
    long processors = sysconf(_SC_NPROCESSORS_ONLN);
    return clamp(static_cast<int>(processors), 1, 8);
}

// The downsampling used for a blur this wide, in pixels: more is cheaper, and
// a wide blur has no detail to lose.
int reduction_for(float sigma)
{
    if (sigma >= 32)
        return 8;
    if (sigma >= 12)
        return 4;
    if (sigma >= 5)
        return 2;
    return 1;
}

using u8x4 = u8 __attribute__((vector_size(4)));
using u32x4 = u32 __attribute__((vector_size(16)));

[[gnu::always_inline]] inline u32x4 widened(Pixel pixel)
{
    u8x4 narrow;
    __builtin_memcpy(&narrow, &pixel, 4);
    return __builtin_convertvector(narrow, u32x4);
}

[[gnu::always_inline]] inline Pixel narrowed(u32x4 wide)
{
    u8x4 narrow = __builtin_convertvector(wide, u8x4);
    Pixel pixel;
    __builtin_memcpy(&pixel, &narrow, 4);
    return pixel;
}

}

Compositor::Compositor(Screen& screen, Theme const& theme, Vector<ServerWindow*> const& windows)
    : m_screen(screen)
    , m_theme(theme)
    , m_windows(windows)
    , m_workers(worker_count())
{
}

ErrorOr<void> Compositor::initialize()
{
    auto size = m_screen.pixels();
    m_tiles_across = (size.width + tile_size - 1) / tile_size;
    m_tiles_down = (size.height + tile_size - 1) / tile_size;
    MUST(m_lowest_shown.try_resize(static_cast<size_t>(m_tiles_across) * static_cast<size_t>(m_tiles_down)));
    TRY(m_dirty.try_resize(static_cast<size_t>(m_tiles_across) * static_cast<size_t>(m_tiles_down)));
    memset(m_dirty.data(), 0, m_dirty.size());
    TRY(render_wallpaper());
    auto* shape = new (Base::nothrow) WindowShape;
    if (!shape)
        return Error::from_errno(ENOMEM);
    auto owned = adopt_own(*shape);
    shape->follows_theme = true;
    TRY(make_shape(*shape));
    m_shapes.append(move(owned));
    invalidate_all();
    return {};
}

ErrorOr<void> Compositor::render_wallpaper()
{
    auto size = m_screen.pixels();
    auto wallpaper = TRY(Bitmap::create(size));
    // An original wallpaper, from the theme's colors: a diagonal gradient under
    // soft rolling shapes.
    Painter painter(*wallpaper);
    auto const& colors = m_wallpaper_colors.is_empty() ? m_theme.wallpaper : m_wallpaper_colors;
    Vector<GradientStop> stops;
    for (size_t i = 0; i < colors.size(); ++i)
        stops.append(GradientStop { colors.size() > 1 ? static_cast<float>(i) / static_cast<float>(colors.size() - 1) : 0, colors[i] });
    auto w = static_cast<float>(size.width);
    auto h = static_cast<float>(size.height);
    Path everything;
    everything.add_rect({ 0, 0, w, h });
    painter.fill_path(everything, Gradient::linear({ 0, 0 }, { w, h }, stops.span()));
    Color const light = Color::from_rgb(0xffffff, 30);
    Color const dark = Color::from_rgb(0x000000, 24);
    struct Hill {
        float base;
        float rise;
        Color color;
    };
    Hill const hills[] = { { 0.62f, 0.26f, light }, { 0.74f, 0.22f, dark }, { 0.86f, 0.18f, light } };
    for (auto const& hill : hills) {
        Path shape;
        shape.move_to({ 0, h * hill.base });
        shape.cubic_to({ w * 0.3f, h * (hill.base - hill.rise) }, { w * 0.6f, h * (hill.base + hill.rise) }, { w, h * (hill.base - hill.rise * 0.4f) });
        shape.line_to({ w, h });
        shape.line_to({ 0, h });
        shape.close();
        painter.fill_path(shape, hill.color);
    }
    m_wallpaper = move(wallpaper);
    return {};
}

ErrorOr<void> Compositor::make_shape(WindowShape& shape)
{
    if (shape.follows_theme)
        shape.radius = static_cast<int>(m_theme.corner_radius);
    float radius = static_cast<float>(shape.radius) * static_cast<float>(m_screen.scale());
    // The extent of a continuous corner along an edge, in radii, rounded up.
    shape.corner_extent = static_cast<int>(Math::ceil(radius * 1.52866483f)) + 1;
    auto mask = TRY(AlphaMask::create({ shape.corner_extent, shape.corner_extent }));
    Path corner;
    auto far = static_cast<float>(shape.corner_extent * 4);
    corner.add_continuous_rounded_rect({ 0, 0, far, far }, radius);
    Rasterizer rasterizer({ 0, 0, shape.corner_extent, shape.corner_extent });
    rasterizer.add_path(corner);
    rasterizer.rasterize(FillRule::NonZero, [&](int y, int x, int count, u8 const* coverage) {
        memcpy(mask->scanline(y) + x, coverage, static_cast<size_t>(count));
    });
    shape.corner = move(mask);
    shape.shadow_active = TRY(make_shadow(shape, m_theme.shadow_active));
    shape.shadow_inactive = TRY(make_shadow(shape, m_theme.shadow_inactive));
    shape.shadow_popup = TRY(make_shadow(shape, m_theme.shadow_popup));
    shape.outline = TRY(make_outline(shape));
    return {};
}

ErrorOr<WindowShape const*> Compositor::shape_with_radius(int points)
{
    for (auto& shape : m_shapes) {
        if (!shape->follows_theme && shape->radius == points)
            return shape.ptr();
    }
    auto* shape = new (Base::nothrow) WindowShape;
    if (!shape)
        return Error::from_errno(ENOMEM);
    auto owned = adopt_own(*shape);
    shape->radius = points;
    TRY(make_shape(*shape));
    m_shapes.append(move(owned));
    return shape;
}

ErrorOr<void> Compositor::theme_changed()
{
    TRY(render_wallpaper());
    for (auto& shape : m_shapes)
        TRY(make_shape(*shape));
    invalidate_all();
    return {};
}

ErrorOr<NineSlice> Compositor::make_shadow(WindowShape const& window_shape, ShadowStyle const& style)
{
    float scale = static_cast<float>(m_screen.scale());
    float sigma = style.blur * scale;
    float radius = static_cast<float>(window_shape.radius) * scale;
    NineSlice slice;
    slice.margin = static_cast<int>(Math::ceil(sigma * 3));
    slice.inset = slice.margin * 2 + window_shape.corner_extent + 2;
    int size = slice.inset * 2 + 4;
    auto mask = TRY(AlphaMask::create({ size, size }));
    Path shape;
    auto margin = static_cast<float>(slice.margin);
    auto inner = static_cast<float>(size) - margin * 2;
    shape.add_continuous_rounded_rect({ margin, margin, inner, inner }, radius);
    Rasterizer rasterizer({ 0, 0, size, size });
    rasterizer.add_path(shape);
    rasterizer.rasterize(FillRule::NonZero, [&](int y, int x, int count, u8 const* coverage) {
        memcpy(mask->scanline(y) + x, coverage, static_cast<size_t>(count));
    });
    TRY(gaussian_blur(*mask, sigma));
    slice.mask = move(mask);
    return slice;
}

ErrorOr<NineSlice> Compositor::make_outline(WindowShape const& shape)
{
    float radius = static_cast<float>(shape.radius) * static_cast<float>(m_screen.scale());
    NineSlice slice;
    slice.margin = 0;
    slice.inset = shape.corner_extent + 2;
    int size = slice.inset * 2 + 4;
    auto mask = TRY(AlphaMask::create({ size, size }));
    Path ring;
    auto extent = static_cast<float>(size);
    ring.add_continuous_rounded_rect({ 0, 0, extent, extent }, radius);
    // One device pixel wide, all the way around, inside the frame.
    ring.add_continuous_rounded_rect({ 1, 1, extent - 2, extent - 2 }, max(radius - 1, 0.0f));
    Rasterizer rasterizer({ 0, 0, size, size });
    rasterizer.add_path(ring);
    rasterizer.rasterize(FillRule::EvenOdd, [&](int y, int x, int count, u8 const* coverage) {
        memcpy(mask->scanline(y) + x, coverage, static_cast<size_t>(count));
    });
    slice.mask = move(mask);
    return slice;
}

ShadowStyle const& Compositor::shadow_style_of(ServerWindow const& window) const
{
    if (window.is_popup())
        return m_theme.shadow_popup;
    return window.active ? m_theme.shadow_active : m_theme.shadow_inactive;
}

NineSlice const& Compositor::shadow_slice_of(ServerWindow const& window) const
{
    if (window.is_popup())
        return window.shape->shadow_popup;
    return window.active ? window.shape->shadow_active : window.shape->shadow_inactive;
}

IntRect Compositor::device_frame(ServerWindow const& window) const
{
    int scale = m_screen.scale();
    return { window.frame.x * scale + window.slide_x, window.frame.y * scale, window.frame.width * scale, window.frame.height * scale };
}

IntRect Compositor::window_bounds(ServerWindow const& window) const
{
    auto frame = device_frame(window);
    if (window.is_bare())
        return frame;
    auto const& style = shadow_style_of(window);
    auto const& slice = shadow_slice_of(window);
    int offset = static_cast<int>(style.offset_y * static_cast<float>(m_screen.scale()));
    auto shadow = frame.inflated(slice.margin).translated(0, offset);
    return shadow.united(frame);
}

void Compositor::invalidate_window(ServerWindow const& window)
{
    invalidate(window_bounds(window));
}

bool Compositor::invalidate_reporting(IntRect area)
{
    area = area.intersected(m_screen.framebuffer().rect());
    if (area.is_empty())
        return false;
    int left = area.x / tile_size;
    int right = (area.right() - 1) / tile_size;
    int top = area.y / tile_size;
    int bottom = (area.bottom() - 1) / tile_size;
    bool changed = false;
    for (int y = top; y <= bottom && !changed; ++y) {
        u8 const* row = m_dirty.data() + static_cast<size_t>(y) * static_cast<size_t>(m_tiles_across);
        for (int x = left; x <= right; ++x) {
            if (!row[x]) {
                changed = true;
                break;
            }
        }
    }
    if (changed)
        invalidate(area);
    return changed;
}

void Compositor::invalidate(IntRect area)
{
    area = area.intersected(m_screen.framebuffer().rect());
    if (area.is_empty())
        return;
    int left = area.x / tile_size;
    int right = (area.right() - 1) / tile_size;
    int top = area.y / tile_size;
    int bottom = (area.bottom() - 1) / tile_size;
    for (int y = top; y <= bottom; ++y)
        memset(m_dirty.data() + static_cast<size_t>(y) * static_cast<size_t>(m_tiles_across) + left, 1, static_cast<size_t>(right - left + 1));
    m_any_dirty = true;
}

void Compositor::invalidate_all()
{
    invalidate(m_screen.framebuffer().rect());
}

// Runs of dirty tiles along each row, with identical runs on the rows below merged.
Vector<IntRect> Compositor::dirty_rects() const
{
    Vector<IntRect> rects;
    auto screen = m_screen.framebuffer().rect();
    // Those that reached the row above.
    Vector<size_t> open;
    for (int y = 0; y < m_tiles_down; ++y) {
        Vector<size_t> still_open;
        u8 const* row = m_dirty.data() + static_cast<size_t>(y) * static_cast<size_t>(m_tiles_across);
        for (int x = 0; x < m_tiles_across;) {
            if (!row[x]) {
                ++x;
                continue;
            }
            int start = x;
            while (x < m_tiles_across && row[x])
                ++x;
            IntRect run { start * tile_size, y * tile_size, (x - start) * tile_size, tile_size };
            bool merged = false;
            for (size_t index : open) {
                auto& above = rects[index];
                if (above.x == run.x && above.width == run.width) {
                    above.height += tile_size;
                    still_open.append(index);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                still_open.append(rects.size());
                rects.append(run);
            }
        }
        open = move(still_open);
    }
    for (auto& rect : rects)
        rect = rect.intersected(screen);
    return rects;
}

bool Compositor::is_drawn(ServerWindow const& window) const
{
    return window.visible && !(m_hides_normal_windows && window.level == Window::LevelNormal);
}

IntRect Compositor::tile_rect(int across, int down) const
{
    return IntRect { across * tile_size, down * tile_size, tile_size, tile_size }.intersected(m_screen.framebuffer().rect());
}

bool Compositor::covers_tile(ServerWindow const& window, IntRect frame, IntRect tile) const
{
    if (tile.x < frame.x || tile.y < frame.y || tile.right() > frame.right() || tile.bottom() > frame.bottom())
        return false;
    // (A surface may not reach the edge of its window, as it's being resized.)
    auto const* surface = window.shown_surface();
    if (surface && (tile.right() - frame.x > surface->width() || tile.bottom() - frame.y > surface->height()))
        return false;
    // The rounded corners show what's behind them.
    if (!window.is_bare() && window.shape) {
        int const extent = window.shape->corner_extent;
        bool const near_an_end = tile.y < frame.y + extent || tile.bottom() > frame.bottom() - extent;
        bool const near_a_side = tile.x < frame.x + extent || tile.right() > frame.right() - extent;
        if (near_an_end && near_a_side)
            return false;
    }
    int const cell = ServerWindow::opaque_cell_size;
    int const first_column = (tile.x - frame.x) / cell;
    int const last_column = (tile.right() - 1 - frame.x) / cell;
    int const first_row = (tile.y - frame.y) / cell;
    int const last_row = (tile.bottom() - 1 - frame.y) / cell;
    int const chrome_height = window.is_titled() && !window.is_unified() && window.chrome_is_opaque && window.chrome ? window.chrome->height() : 0;
    for (int row = first_row; row <= last_row; ++row) {
        // The title bar goes over all of what's under it.
        if ((row + 1) * cell <= chrome_height)
            continue;
        if (row >= window.opaque_cell_rows || last_column >= window.opaque_cell_columns || !window.shown_surface())
            return false;
        u8 const* cells = window.opaque_cells.data() + static_cast<size_t>(row) * static_cast<size_t>(window.opaque_cell_columns);
        for (int column = first_column; column <= last_column; ++column) {
            if (!cells[column])
                return false;
        }
    }
    return true;
}

void Compositor::find_what_shows()
{
    int const scale = m_screen.scale();
    size_t const tile_count = m_lowest_shown.size();
    for (size_t i = 0; i < tile_count; ++i)
        m_lowest_shown[i] = -1;
    // From the top: the first to cover a tile is the lowest that shows there.
    for (size_t index = m_windows.size(); index > 0; --index) {
        auto const& window = *m_windows[index - 1];
        if (!is_drawn(window))
            continue;
        auto const frame = device_frame(window);
        auto const on_screen = frame.intersected(m_screen.framebuffer().rect());
        if (on_screen.is_empty())
            continue;
        for (int down = on_screen.y / tile_size; down <= (on_screen.bottom() - 1) / tile_size; ++down) {
            auto* row = m_lowest_shown.data() + static_cast<size_t>(down) * static_cast<size_t>(m_tiles_across);
            for (int across = on_screen.x / tile_size; across <= (on_screen.right() - 1) / tile_size; ++across) {
                if (row[across] < 0 && covers_tile(window, frame, tile_rect(across, down)))
                    row[across] = static_cast<i16>(index - 1);
            }
        }
    }
    // A material shows wherever nothing opaque is over it, itself included. Then all that it reads has to be there to
    // read, under this window, even where more is over that in the end. Which may bring lower materials into view: so from
    // the top down, and each of them sees what the ones above have asked for.
    m_shown_materials.clear();
    for (size_t index = m_windows.size(); index > 0; --index) {
        auto const& window = *m_windows[index - 1];
        if (!is_drawn(window) || window.materials.is_empty())
            continue;
        int const at = static_cast<int>(index - 1);
        auto const frame = device_frame(window);
        for (size_t material_index = 0; material_index < window.materials.size(); ++material_index) {
            auto const& material = window.materials[material_index];
            IntRect region { frame.x + material.rect.x * scale, frame.y + material.rect.y * scale, material.rect.width * scale, material.rect.height * scale };
            region = region.intersected(frame);
            auto const on_screen = region.intersected(m_screen.framebuffer().rect());
            if (on_screen.is_empty())
                continue;
            IntRect shown;
            for (int down = on_screen.y / tile_size; down <= (on_screen.bottom() - 1) / tile_size; ++down) {
                auto const* row = m_lowest_shown.data() + static_cast<size_t>(down) * static_cast<size_t>(m_tiles_across);
                for (int across = on_screen.x / tile_size; across <= (on_screen.right() - 1) / tile_size; ++across) {
                    if (row[across] < at)
                        shown = shown.is_empty() ? tile_rect(across, down) : shown.united(tile_rect(across, down));
                }
            }
            shown = shown.intersected(on_screen);
            if (shown.is_empty())
                continue;
            auto const& style = m_theme.materials[static_cast<int>(material.material)];
            float const sigma = style.blur * static_cast<float>(scale);
            int const reach = static_cast<int>(Math::ceil(sigma * 3)) + reduction_for(sigma) * 2;
            m_shown_materials.append(ShownMaterial { index - 1, material_index, region, shown, reach });

            auto const around = shown.inflated(reach).intersected(m_screen.framebuffer().rect());
            for (int down = around.y / tile_size; down <= (around.bottom() - 1) / tile_size; ++down) {
                auto* row = m_lowest_shown.data() + static_cast<size_t>(down) * static_cast<size_t>(m_tiles_across);
                for (int across = around.x / tile_size; across <= (around.right() - 1) / tile_size; ++across) {
                    if (row[across] < at)
                        continue;
                    // The nearest below this window that covers it, if anything does.
                    auto const tile = tile_rect(across, down);
                    int lowest = -1;
                    for (int below = at - 1; below >= 0; --below) {
                        auto const& under = *m_windows[static_cast<size_t>(below)];
                        if (is_drawn(under) && covers_tile(under, device_frame(under), tile)) {
                            lowest = below;
                            break;
                        }
                    }
                    row[across] = static_cast<i16>(lowest);
                }
            }
        }
    }
}

void Compositor::grow_damage_for_materials()
{
    // A material reads what is below it, up to its blur's reach away, and the
    // screen only holds that where it is being redrawn this frame (elsewhere it
    // holds the finished picture, this very window and those above it included).
    // So if anything changed within reach, all of the material is redone, and all
    // that it reads is drawn again. Which may make dirty what another material must
    // have whole, above it or below: so round again, until nothing more comes of it.
    for (bool grew = true; grew;) {
        grew = false;
        for (auto const& material : m_shown_materials) {
            auto const around = material.shown.inflated(material.reach);
            for (auto const& rect : dirty_rects()) {
                if (around.intersects(rect)) {
                    if (invalidate_reporting(around))
                        grew = true;
                    break;
                }
            }
        }
    }
}

Vector<Compositor::Piece> Compositor::pieces_of(Vector<IntRect> const& rects) const
{
    Vector<Piece> pieces;
    for (auto const& rect : rects) {
        // A row of tiles at a time, cut wherever what shows there changes.
        for (int down = rect.y / tile_size; down <= (rect.bottom() - 1) / tile_size; ++down) {
            int const top = max(rect.y, down * tile_size);
            int const bottom = min(rect.bottom(), (down + 1) * tile_size);
            auto const* row = m_lowest_shown.data() + static_cast<size_t>(down) * static_cast<size_t>(m_tiles_across);
            int const last = (rect.right() - 1) / tile_size;
            for (int across = rect.x / tile_size; across <= last;) {
                int const lowest = row[across];
                int next = across + 1;
                // (Not too wide, as a material only needs some of them first.)
                while (next <= last && row[next] == lowest && next - across < 16)
                    ++next;
                int const left = max(rect.x, across * tile_size);
                int const right = min(rect.right(), next * tile_size);
                pieces.append(Piece { IntRect { left, top, right - left, bottom - top }, lowest });
                across = next;
            }
        }
    }
    return pieces;
}

void Compositor::draw_steps(Vector<Step> const& steps, size_t end, Vector<Piece*> const& pieces)
{
    if (pieces.is_empty())
        return;
    StepJob work { &steps, end, &pieces, this };
    m_workers.run(pieces.size(), [](void* context, size_t index) {
        auto const& job = *static_cast<StepJob*>(context);
        auto& piece = *(*job.pieces)[index];
        for (size_t i = piece.next_step; i < job.end; ++i) {
            auto const& step = (*job.steps)[i];
            switch (step.kind) {
            case StepKind::Wallpaper:
                if (piece.lowest < 0)
                    job.compositor->draw_wallpaper(piece.rect);
                break;
            case StepKind::Shadow:
                // Under the window it's of, if that covers this.
                if (step.index > piece.lowest)
                    job.compositor->draw_shadow(*step.window, piece.rect);
                break;
            case StepKind::Body:
                if (step.index >= piece.lowest)
                    job.compositor->draw_body(*step.window, piece.rect, step.index == piece.lowest);
                break;
            }
        }
        piece.next_step = job.end;
    },
        &work);
}

void Compositor::surface_replaced(ServerWindow& window)
{
    auto const* first = window.buffers[0].ptr();
    int const cell = ServerWindow::opaque_cell_size;
    window.opaque_cell_columns = first ? (first->width() + cell - 1) / cell : 0;
    window.opaque_cell_rows = first ? (first->height() + cell - 1) / cell : 0;
    MUST(window.opaque_cells.try_resize(static_cast<size_t>(window.opaque_cell_columns) * static_cast<size_t>(window.opaque_cell_rows)));
    for (auto& opaque : window.opaque_cells)
        opaque = 0;
}

void Compositor::surface_changed(ServerWindow& window, IntRect damage)
{
    auto const* surface = window.shown_surface();
    if (!surface || window.opaque_cells.is_empty())
        return;
    damage = damage.intersected(surface->rect());
    if (damage.is_empty())
        return;
    int const cell = ServerWindow::opaque_cell_size;
    struct Scan {
        ServerWindow* window;
        Bitmap const* surface;
        int first_column;
        int last_column;
        int first_row;
    } scan { &window, surface, damage.x / cell, (damage.right() - 1) / cell, damage.y / cell };
    int const rows = (damage.bottom() - 1) / cell - scan.first_row + 1;
    // Most of this is one small part of a window. All of a big one would be seen, but for the other processors.
    m_workers.run(static_cast<size_t>(rows), [](void* context, size_t index) {
        auto const& job = *static_cast<Scan*>(context);
        int const cell = ServerWindow::opaque_cell_size;
        int const row = job.first_row + static_cast<int>(index);
        int const top = row * cell;
        int const bottom = min(top + cell, job.surface->height());
        u8* cells = job.window->opaque_cells.data() + static_cast<size_t>(row) * static_cast<size_t>(job.window->opaque_cell_columns);
        for (int column = job.first_column; column <= job.last_column; ++column) {
            int const left = column * cell;
            int const count = min(left + cell, job.surface->width()) - left;
            bool opaque = true;
            for (int y = top; y < bottom && opaque; ++y) {
                auto const* pixels = job.surface->scanline(y) + left;
                u32 together = 0xffffffffu;
                for (int x = 0; x < count; ++x)
                    together &= static_cast<u32>(pixel_alpha(pixels[x]));
                opaque = (together & 0xff) == 0xff;
            }
            cells[column] = opaque;
        }
    },
        &scan);
}

void Compositor::draw_wallpaper(IntRect clip)
{
    auto& framebuffer = m_screen.framebuffer();
    for (int y = clip.y; y < clip.bottom(); ++y)
        memcpy(framebuffer.scanline(y) + clip.x, m_wallpaper->scanline(y) + clip.x, static_cast<size_t>(clip.width) * sizeof(Pixel));
}

void Compositor::draw_nine_slice(NineSlice const& slice, IntRect target, Color color, IntRect clip, IntRect hole, int hole_corner)
{
    auto& framebuffer = m_screen.framebuffer();
    auto area = target.intersected(clip);
    if (area.is_empty() || !slice.mask)
        return;
    Pixel value = color.to_pixel();
    auto const& mask = *slice.mask;
    int size = mask.width();
    int inset = slice.inset;
    int right_start = max(target.right() - inset, target.x + inset);
    for (int y = area.y; y < area.bottom(); ++y) {
        // The mask's row for this one.
        int from_top = y - target.y;
        int from_bottom = target.bottom() - 1 - y;
        int mask_y = from_top < inset ? from_top : (from_bottom < inset ? size - 1 - from_bottom : inset);
        // Where it does not go: under the window, but for the corners, where
        // the rounded edge shows what is behind it.
        int hole_left = area.right(), hole_right = area.right();
        if (!hole.is_empty() && y >= hole.y && y < hole.bottom()) {
            bool in_corner_row = y < hole.y + hole_corner || y >= hole.bottom() - hole_corner;
            hole_left = in_corner_row ? hole.x + hole_corner : hole.x;
            hole_right = in_corner_row ? hole.right() - hole_corner : hole.right();
        }
        u8 const* mask_row = mask.scanline(mask_y);
        auto* row = framebuffer.scanline(y);
        auto draw_span = [&](int from, int to) {
            if (from >= to)
                return;
            // Left part, middle (one value repeated), right part.
            int left_end = min(to, target.x + inset);
            if (from < left_end)
                blend_row_solid_coverage(row + from, value, mask_row + (from - target.x), left_end - from);
            int middle_start = max(from, target.x + inset);
            int middle_end = min(to, right_start);
            if (middle_start < middle_end) {
                u8 coverage = mask_row[inset];
                if (coverage)
                    blend_row_solid(row + middle_start, scale_pixel(value, coverage), middle_end - middle_start);
            }
            int right_from = max(from, right_start);
            if (right_from < to)
                blend_row_solid_coverage(row + right_from, value, mask_row + (size - (target.right() - right_from)), to - right_from);
        };
        draw_span(area.x, min(hole_left, area.right()));
        draw_span(max(hole_right, area.x), area.right());
    }
}

void Compositor::draw_shadow(ServerWindow const& window, IntRect clip)
{
    if (window.is_bare())
        return;
    auto const& style = shadow_style_of(window);
    auto const& slice = shadow_slice_of(window);
    auto frame = device_frame(window);
    int offset = static_cast<int>(style.offset_y * static_cast<float>(m_screen.scale()));
    draw_nine_slice(slice, frame.inflated(slice.margin).translated(0, offset), style.color, clip, frame, window.shape->corner_extent);
}

void Compositor::blend_row_in_window(Pixel* destination, Pixel const* source, int x, int y, int count, IntSize frame_size, WindowShape const* shape) const
{
    int extent = shape ? shape->corner_extent : 0;
    bool corner_row = shape && (y < extent || y >= frame_size.height - extent);
    if (!corner_row) {
        blend_row(destination, source, count);
        return;
    }
    int mask_y = y < extent ? y : frame_size.height - 1 - y;
    u8 const* mask_row = shape->corner->scanline(mask_y);
    int end = x + count;
    // The left corner, the middle, and the right corner, mirrored.
    int left_end = min(end, extent);
    if (x < left_end)
        blend_row_coverage(destination, source, mask_row + x, left_end - x);
    int middle_from = max(x, extent);
    int middle_to = min(end, frame_size.width - extent);
    if (middle_from < middle_to)
        blend_row(destination + (middle_from - x), source + (middle_from - x), middle_to - middle_from);
    int right_from = max(x, frame_size.width - extent);
    if (right_from < end) {
        u8 mirrored[128];
        int length = end - right_from;
        if (length > static_cast<int>(sizeof(mirrored)))
            return;
        for (int i = 0; i < length; ++i)
            mirrored[i] = mask_row[frame_size.width - 1 - (right_from + i)];
        blend_row_coverage(destination + (right_from - x), source + (right_from - x), mirrored, length);
    }
}

void Compositor::draw_body(ServerWindow const& window, IntRect clip, bool covers_it)
{
    auto& framebuffer = m_screen.framebuffer();
    auto frame = device_frame(window);
    auto area = frame.intersected(clip);
    if (area.is_empty())
        return;
    auto const* shape = window.is_bare() ? nullptr : window.shape;
    IntSize frame_size = frame.size();
    auto const* surface = window.shown_surface();
    Pixel background = m_theme.window_background.to_pixel();
    auto const* chrome = window.is_titled() ? window.chrome.ptr() : nullptr;
    // A row of the background, for where there is no surface (yet).
    Pixel fill[256];
    bool filled = false;
    for (int y = area.y; y < area.bottom(); ++y) {
        int row_in_frame = y - frame.y;
        auto* destination = framebuffer.scanline(y) + area.x;
        int x_in_frame = area.x - frame.x;
        // Client pixels where it has some, the window's background where not.
        // (No background under a surface: where it is clear, a material shows.)
        // (The title bar covers it all, and blending both through a rounded
        // corner would let the client's pixels show at the edge.)
        // (A unified title bar, though, is drawn by the client, and goes over it.)
        bool in_chrome = chrome && row_in_frame < chrome->height();
        bool under_chrome = in_chrome && !window.is_unified();
        // All of it, and no blending to do.
        if (covers_it && !in_chrome) {
            memcpy(destination, surface->scanline(row_in_frame) + x_in_frame, static_cast<size_t>(area.width) * sizeof(Pixel));
            continue;
        }
        int covered_to = area.width;
        if (under_chrome) {
            covered_to = area.width;
        } else if (surface && row_in_frame < surface->height()) {
            covered_to = clamp(surface->width() - x_in_frame, 0, area.width);
            if (covered_to > 0)
                blend_row_in_window(destination, surface->scanline(row_in_frame) + x_in_frame, x_in_frame, row_in_frame, covered_to, frame_size, shape);
        } else {
            covered_to = 0;
        }
        for (int done = covered_to; done < area.width;) {
            if (!filled) {
                for (auto& pixel : fill)
                    pixel = background;
                filled = true;
            }
            int chunk = min(area.width - done, static_cast<int>(array_size(fill)));
            blend_row_in_window(destination + done, fill, x_in_frame + done, row_in_frame, chunk, frame_size, shape);
            done += chunk;
        }
        if (in_chrome) {
            int width = min(area.width, chrome->width() - x_in_frame);
            if (width > 0)
                blend_row_in_window(destination, chrome->scanline(row_in_frame) + x_in_frame, x_in_frame, row_in_frame, width, frame_size, shape);
        }
    }
    if (shape)
        draw_nine_slice(shape->outline, frame, m_theme.outline, clip, frame.inflated(-1), shape->corner_extent);
}

void Compositor::read_material_rows(MaterialJob const& job, int first, int end)
{
    auto& framebuffer = m_screen.framebuffer();
    // Down: each small pixel the average of what it covers (of every other
    // pixel, at the widest blurs, which leave no trace of the difference).
    int const reduction = job.reduction;
    int stride = reduction >= 8 ? 2 : 1;
    int per_side = reduction / stride;
    u32 reciprocal = 65536u / static_cast<u32>(per_side * per_side);
    for (int y = first; y < end; ++y) {
        auto* out = job.small->scanline(y);
        for (int x = 0; x < job.small_width; ++x) {
            u32x4 sum {};
            for (int sy = 0; sy < per_side; ++sy) {
                auto const* row = framebuffer.scanline(min(job.grid_top + y * reduction + sy * stride, framebuffer.height() - 1));
                for (int sx = 0; sx < per_side; ++sx)
                    sum += widened(row[min(job.grid_left + x * reduction + sx * stride, framebuffer.width() - 1)]);
            }
            out[x] = narrowed((sum * reciprocal + 32768u) >> 16);
        }
    }
}

void Compositor::blur_material(MaterialJob const& job)
{
    IntRect const region { 0, 0, job.small_width, job.small_height };
    if (gaussian_blur(*job.small, region, job.sigma / static_cast<float>(job.reduction), job.spare).is_error())
        return;
    adjust_saturation(*job.small, region, job.style->saturation);
}

void Compositor::place_material_rows(MaterialJob const& job, int first, int end)
{
    // Back up: bilinear (a row between two small rows, then across it), then the tint.
    auto const& area = job.area;
    int const reduction = job.reduction;
    int const small_width = job.small_width;
    int const small_height = job.small_height;
    Pixel tint = job.style->tint.to_pixel();
    Vector<Pixel> between;
    Vector<Pixel> row_buffer;
    if (between.try_resize(static_cast<size_t>(small_width)).is_error() || row_buffer.try_resize(static_cast<size_t>(area.width)).is_error())
        return;
    int half = reduction / 2;
    u32 step = 256u / static_cast<u32>(reduction);
    for (int y = first; y < end; ++y) {
        int position_y = max(y - job.grid_top - half, 0);
        int upper = min(position_y / reduction, small_height - 1);
        int lower = min(upper + 1, small_height - 1);
        u32 fraction_y = static_cast<u32>(position_y % reduction) * step;
        auto const* top = job.small->scanline(upper);
        auto const* bottom = job.small->scanline(lower);
        for (int x = 0; x < small_width; ++x)
            between[static_cast<size_t>(x)] = narrowed((widened(top[x]) * (256 - fraction_y) + widened(bottom[x]) * fraction_y + 128u) >> 8);

        int position_x = max(area.x - job.grid_left - half, 0);
        int offset = position_x % reduction;
        int column = position_x / reduction;
        int written = 0;
        while (written < area.width) {
            u32x4 left = widened(between[static_cast<size_t>(min(column, small_width - 1))]);
            u32x4 right = widened(between[static_cast<size_t>(min(column + 1, small_width - 1))]);
            int run = min(reduction - offset, area.width - written);
            for (int i = 0; i < run; ++i) {
                u32 fraction_x = static_cast<u32>(offset + i) * step;
                row_buffer[static_cast<size_t>(written + i)] = narrowed((left * (256 - fraction_x) + right * fraction_x + 128u) >> 8);
            }
            written += run;
            offset = 0;
            ++column;
        }
        blend_row_solid(row_buffer.data(), tint, area.width);
        place_material_row(job, y, row_buffer.data());
    }
}

void Compositor::place_material_row(MaterialJob const& job, int y, Pixel const* source)
{
    auto& framebuffer = m_screen.framebuffer();
    auto const& area = job.area;
    auto const& window = *job.window;
    auto const& region = job.region;
    int const radius = job.radius;
    auto frame = device_frame(window);
    if (radius <= 0) {
        blend_row_in_window(framebuffer.scanline(y) + area.x, source, area.x - frame.x, y - frame.y, area.width, frame.size(), window.is_bare() ? nullptr : window.shape);
        return;
    }
    // Its own rounded corners: rows near the top and bottom start and end further in,
    // with the edge's pixel covered as far as the circle goes.
    auto r = static_cast<float>(radius);
    int from_top = y - region.y;
    int from_bottom = region.bottom() - 1 - y;
    int nearest = min(from_top, from_bottom);
    float inset = 0;
    if (nearest < radius) {
        float dy = r - (static_cast<float>(nearest) + 0.5f);
        inset = r - static_cast<float>(Math::square_root(static_cast<double>(r * r - dy * dy)));
    }
    int whole = static_cast<int>(inset);
    auto* row = framebuffer.scanline(y);
    int start = region.x + whole;
    int end = region.right() - whole;
    if (start >= end)
        return;
    auto put = [&](int from, int to, u32 opacity) {
        from = max(from, area.x);
        to = min(to, area.right());
        if (from >= to)
            return;
        if (opacity >= 255)
            blend_row(row + from, source + (from - area.x), to - from);
        else
            blend_row_opacity(row + from, source + (from - area.x), opacity, to - from);
    };
    if (inset > 0) {
        auto edge = static_cast<u32>((1 - (inset - static_cast<float>(whole))) * 255);
        put(start, start + 1, edge);
        put(end - 1, end, edge);
        put(start + 1, end - 1, 255);
    } else {
        put(start, end, 255);
    }
}

void Compositor::draw_materials(Vector<MaterialJob>& jobs)
{
    auto const& framebuffer = m_screen.framebuffer();
    // What to read, on a grid of the reduction, so the same pixels come out
    // whichever part is being redrawn: nothing shimmers as things move.
    while (m_material_memory.size() < jobs.size())
        m_material_memory.append({});
    Vector<MaterialBand> reading;
    Vector<MaterialBand> placing;
    for (size_t i = 0; i < jobs.size(); ++i) {
        auto& job = jobs[i];
        job.sigma = job.style->blur * static_cast<float>(m_screen.scale());
        job.reduction = reduction_for(job.sigma);
        int reach = static_cast<int>(Math::ceil(job.sigma * 3)) + job.reduction * 2;
        job.source = job.area.inflated(reach).intersected(framebuffer.rect());
        job.grid_left = job.source.x / job.reduction * job.reduction;
        job.grid_top = job.source.y / job.reduction * job.reduction;
        job.small_width = (job.source.right() - job.grid_left + job.reduction - 1) / job.reduction;
        job.small_height = (job.source.bottom() - job.grid_top + job.reduction - 1) / job.reduction;
        auto& memory = m_material_memory[i];
        if (!memory.small || memory.small->width() < job.small_width || memory.small->height() < job.small_height) {
            // A little more than it needs, for it not to happen every time something grows by a row.
            IntSize const wanted { job.small_width + 32, job.small_height + 32 };
            auto small = Bitmap::create(wanted);
            auto spare = Bitmap::create(wanted);
            if (small.is_error() || spare.is_error())
                continue;
            memory.small = small.release_value();
            memory.spare = spare.release_value();
        }
        job.small = memory.small.ptr();
        job.spare = memory.spare.ptr();
        for (int y = 0; y < job.small_height; y += 8)
            reading.append(MaterialBand { &job, y, min(y + 8, job.small_height) });
        for (int y = job.area.y; y < job.area.bottom(); y += 16)
            placing.append(MaterialBand { &job, y, min(y + 16, job.area.bottom()) });
    }
    // All from what is below, none reading what another wrote, whichever thread gets there first; and only then into the screen.
    u64 const started = now_microseconds();
    m_workers.run(reading.size(), [](void* context, size_t index) {
        auto const& band = static_cast<MaterialBand const*>(context)[index];
        band.job->compositor->read_material_rows(*band.job, band.first, band.end);
    },
        reading.data());
    m_workers.run(jobs.size(), [](void* context, size_t index) {
        auto const& job = static_cast<MaterialJob const*>(context)[index];
        if (job.small)
            job.compositor->blur_material(job);
    },
        jobs.data());
    m_material_microseconds += now_microseconds() - started;
    m_workers.run(placing.size(), [](void* context, size_t index) {
        auto const& band = static_cast<MaterialBand const*>(context)[index];
        band.job->compositor->place_material_rows(*band.job, band.first, band.end);
    },
        placing.data());
}

void Compositor::set_overlays(Vector<Overlay> overlays)
{
    m_overlays = move(overlays);
}

void Compositor::set_dim(Color color)
{
    m_dim = color;
}

void Compositor::set_wallpaper_colors(Vector<Color> colors)
{
    m_wallpaper_colors = move(colors);
    if (!render_wallpaper().is_error())
        invalidate_all();
}

ErrorOr<NonnullRefPtr<Bitmap>> Compositor::snapshot(ServerWindow const& window) const
{
    auto frame = device_frame(window);
    auto picture = TRY(Bitmap::create(frame.size()));
    auto const* surface = window.shown_surface();
    Pixel background = m_theme.window_background.to_pixel();
    auto const* chrome = window.is_titled() ? window.chrome.ptr() : nullptr;
    auto const* shape = window.is_bare() ? nullptr : window.shape;
    Vector<Pixel> fill;
    MUST(fill.try_resize(static_cast<size_t>(frame.width)));
    for (auto& pixel : fill)
        pixel = background;
    for (int y = 0; y < frame.height; ++y) {
        auto* row = picture->scanline(y);
        bool in_chrome = chrome && y < chrome->height();
        bool under_chrome = in_chrome && !window.is_unified();
        blend_row_in_window(row, fill.data(), 0, y, frame.width, frame.size(), shape);
        if (!under_chrome && surface && y < surface->height())
            blend_row_in_window(row, surface->scanline(y), 0, y, min(surface->width(), frame.width), frame.size(), shape);
        if (in_chrome)
            blend_row_in_window(row, chrome->scanline(y), 0, y, min(chrome->width(), frame.width), frame.size(), shape);
    }
    return picture;
}

void Compositor::draw_overlays(Vector<IntRect> const& rects)
{
    auto& framebuffer = m_screen.framebuffer();
    float scale = static_cast<float>(m_screen.scale());
    for (auto const& rect : rects) {
        Painter painter(framebuffer);
        painter.clip_rect(rect.to<float>());
        if (m_dim.alpha)
            painter.fill_rect(rect.to<float>(), m_dim);
        for (auto const& overlay : m_overlays) {
            switch (overlay.kind) {
            case Overlay::Kind::Tile: {
                FloatRect shape { overlay.rect.x * scale, overlay.rect.y * scale, overlay.rect.width * scale, overlay.rect.height * scale };
                painter.fill_rounded_rect(shape, overlay.radius * scale, overlay.color);
                if (!overlay.label.is_empty() && m_overlay_font) {
                    float text_width = m_overlay_font->width_of(overlay.label.view());
                    FloatPoint at { shape.x + (shape.width - text_width) / 2, shape.y + shape.height / 2 + m_overlay_font->cap_height() / 2 };
                    at.y = static_cast<float>(Math::round(static_cast<double>(at.y)));
                    m_overlay_font->draw(painter, at, overlay.label.view(), Color { 255, 255, 255, 255 });
                }
                break;
            }
            case Overlay::Kind::Logo: {
                // Wings, swept up into a V: original, and simple enough for any size.
                FloatRect shape { overlay.rect.x * scale, overlay.rect.y * scale, overlay.rect.width * scale, overlay.rect.height * scale };
                auto at = [&](float x, float y) { return FloatPoint { shape.x + x * shape.width, shape.y + y * shape.height }; };
                Path path;
                path.move_to(at(0.02f, 0.14f));
                path.cubic_to(at(0.16f, 0.30f), at(0.30f, 0.34f), at(0.40f, 0.42f));
                path.line_to(at(0.50f, 0.96f));
                path.line_to(at(0.60f, 0.42f));
                path.cubic_to(at(0.70f, 0.34f), at(0.84f, 0.30f), at(0.98f, 0.14f));
                path.cubic_to(at(0.90f, 0.52f), at(0.72f, 0.62f), at(0.60f, 0.70f));
                path.line_to(at(0.50f, 0.96f));
                path.line_to(at(0.40f, 0.70f));
                path.cubic_to(at(0.28f, 0.62f), at(0.10f, 0.52f), at(0.02f, 0.14f));
                path.close();
                painter.fill_path(path, overlay.color);
                break;
            }
            case Overlay::Kind::Thumbnail: {
                if (!overlay.picture)
                    break;
                FloatRect shape { overlay.rect.x * scale, overlay.rect.y * scale, overlay.rect.width * scale, overlay.rect.height * scale };
                if (overlay.shadow) {
                    for (int i = 0; i < 6; ++i) {
                        auto grown = static_cast<float>(6 - i) * 3;
                        painter.fill_rounded_rect({ shape.x - grown, shape.y - grown / 2, shape.width + grown * 2, shape.height + grown * 2 }, overlay.radius * scale + grown, Color { 0, 0, 0, 8 });
                    }
                }
                painter.draw_bitmap(*overlay.picture, shape);
                break;
            }
            case Overlay::Kind::Genie: {
                if (!overlay.picture)
                    break;
                // Rows between a top and bottom that race down to the target: the bottom
                // first; each narrowed the more the nearer it has come.
                FloatRect from { overlay.rect.x * scale, overlay.rect.y * scale, overlay.rect.width * scale, overlay.rect.height * scale };
                FloatPoint to { overlay.target.x * scale, overlay.target.y * scale };
                float t = clamp(overlay.progress, 0.0f, 1.0f);
                float bottom_t = min(t * 1.6f, 1.0f);
                float top_t = t * t;
                float bottom = from.bottom() + (to.y - from.bottom()) * bottom_t;
                float top = from.y + (to.y - from.y) * top_t;
                float span = bottom - top;
                if (span < 1)
                    break;
                int first = max(static_cast<int>(top), rect.y);
                int last = min(static_cast<int>(bottom) + 1, rect.bottom());
                float funnel = max(to.y - from.y, 1.0f);
                float tip = 8 * scale;
                auto const& picture = *overlay.picture;
                for (int y = first; y < last; ++y) {
                    float v = (static_cast<float>(y) + 0.5f - top) / span;
                    if (v < 0 || v >= 1)
                        continue;
                    int source_y = min(static_cast<int>(v * static_cast<float>(picture.height())), picture.height() - 1);
                    float along = clamp((static_cast<float>(y) - from.y) / funnel, 0.0f, 1.0f);
                    float squeeze = along * along * (3 - 2 * along) * min(t * 2.2f, 1.0f);
                    float width = from.width + (tip - from.width) * squeeze;
                    float center = (from.x + from.width / 2) + (to.x - (from.x + from.width / 2)) * squeeze;
                    int left = static_cast<int>(center - width / 2);
                    int columns = static_cast<int>(width);
                    if (columns <= 0)
                        continue;
                    auto const* source = picture.scanline(source_y);
                    auto* out = framebuffer.scanline(y);
                    int start = max(left, rect.x);
                    int end = min(left + columns, rect.right());
                    u32 step = (static_cast<u32>(picture.width()) << 16) / static_cast<u32>(columns);
                    u32 position = static_cast<u32>(start - left) * step;
                    for (int x = start; x < end; ++x) {
                        Pixel pixel = source[min(position >> 16, static_cast<u32>(picture.width() - 1))];
                        position += step;
                        u32 alpha = pixel_alpha(pixel);
                        if (alpha == 255) {
                            out[x] = pixel;
                        } else if (alpha) {
                            u32 inverse = 255 - alpha;
                            Pixel under = out[x];
                            out[x] = make_pixel(static_cast<u8>(pixel_red(pixel) + divide_by_255(pixel_red(under) * inverse)),
                                static_cast<u8>(pixel_green(pixel) + divide_by_255(pixel_green(under) * inverse)),
                                static_cast<u8>(pixel_blue(pixel) + divide_by_255(pixel_blue(under) * inverse)),
                                static_cast<u8>(alpha + divide_by_255(pixel_alpha(under) * inverse)));
                        }
                    }
                }
                break;
            }
            }
        }
    }
}

void Compositor::render()
{
    if (!m_any_dirty)
        return;
    u64 started = now_microseconds();
    find_what_shows();
    grow_damage_for_materials();
    auto rects = dirty_rects();
    auto pieces = pieces_of(rects);
    Vector<Piece*> every_piece;
    for (auto& piece : pieces)
        every_piece.append(&piece);

    // Bottom to top. A material needs what's below it done first, but only where it reads: the rest waits for the end, and
    // only there do the other processors all get to work on it at once.
    Vector<Step> steps;
    steps.append(Step { StepKind::Wallpaper, nullptr, -1 });
    int const scale = m_screen.scale();
    bool overlays_drawn = false;
    auto draw_overlays_now = [&] {
        overlays_drawn = true;
        if (m_overlays.is_empty() && m_dim.alpha == 0)
            return;
        draw_steps(steps, steps.size(), every_piece);
        draw_overlays(rects);
    };
    for (size_t index = 0; index < m_windows.size(); ++index) {
        auto const* window = m_windows[index];
        if (!overlays_drawn && window->level >= Window::LevelDock)
            draw_overlays_now();
        if (!is_drawn(*window))
            continue;
        steps.append(Step { StepKind::Shadow, window, static_cast<int>(index) });
        Vector<MaterialJob> jobs;
        for (auto const& shown : m_shown_materials) {
            if (shown.window_index != index)
                continue;
            auto const& material = window->materials[shown.material_index];
            auto const& style = m_theme.materials[static_cast<int>(material.material)];
            for (auto const& rect : rects) {
                auto area = shown.shown.intersected(rect);
                if (!area.is_empty())
                    jobs.append(MaterialJob { this, window, &style, area, shown.region, material.radius * scale, {}, 0, 0, 0, 0, 1, 0, nullptr, nullptr });
            }
        }
        if (!jobs.is_empty()) {
            Vector<Piece*> wanted;
            for (auto& piece : pieces) {
                for (auto const& job : jobs) {
                    auto const& style = *job.style;
                    float const sigma = style.blur * static_cast<float>(scale);
                    if (piece.rect.intersects(job.area.inflated(static_cast<int>(Math::ceil(sigma * 3)) + reduction_for(sigma) * 2))) {
                        wanted.append(&piece);
                        break;
                    }
                }
            }
            draw_steps(steps, steps.size(), wanted);
            draw_materials(jobs);
        }
        steps.append(Step { StepKind::Body, window, static_cast<int>(index) });
    }
    if (!overlays_drawn)
        draw_overlays_now();
    draw_steps(steps, steps.size(), every_piece);
    u64 composed = now_microseconds();

    // A flush costs far more for being a flush than for how much it has to show: two or three small ones take longer than
    // one of the whole screen. Only two things a long way apart are worth keeping apart.
    u64 pixel_count = 0;
    IntRect everything;
    u64 separately = 0;
    for (auto const& rect : rects) {
        everything = everything.is_empty() ? rect : everything.united(rect);
        separately += static_cast<u64>(rect.width) * static_cast<u64>(rect.height);
    }
    u64 const together = static_cast<u64>(everything.width) * static_cast<u64>(everything.height);
    if (rects.size() > 2 || together <= separately * 8) {
        m_screen.flush(everything);
        pixel_count = together;
    } else {
        for (auto const& rect : rects)
            m_screen.flush(rect);
        pixel_count = separately;
    }
    u64 flushed = now_microseconds();

    memset(m_dirty.data(), 0, m_dirty.size());
    m_any_dirty = false;
    ++m_frames;
    m_compose_microseconds += composed - started;
    m_flush_microseconds += flushed - composed;
    m_pixels += pixel_count;
}

}
