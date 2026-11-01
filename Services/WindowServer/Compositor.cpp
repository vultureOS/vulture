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
    auto const& colors = m_theme.wallpaper;
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
    return { window.frame.x * scale, window.frame.y * scale, window.frame.width * scale, window.frame.height * scale };
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

void Compositor::grow_damage_for_materials()
{
    int scale = m_screen.scale();
    // A material reads what is below it, up to its blur's reach away, and the
    // screen only holds that where it is being redrawn this frame (elsewhere it
    // holds the finished picture, this very window and those above it included).
    // So if anything changed within reach, all of the material is redone, and all
    // that it reads is drawn again. From the top down: a material lower down may
    // read what this one's reading makes dirty.
    for (size_t index = m_windows.size(); index > 0; --index) {
        auto const& window = *m_windows[index - 1];
        if (!window.visible || window.materials.is_empty())
            continue;
        auto frame = device_frame(window);
        auto rects = dirty_rects();
        for (auto const& material : window.materials) {
            IntRect region { frame.x + material.rect.x * scale, frame.y + material.rect.y * scale, material.rect.width * scale, material.rect.height * scale };
            region = region.intersected(frame);
            if (region.is_empty())
                continue;
            auto const& style = m_theme.materials[static_cast<int>(material.material)];
            float sigma = style.blur * static_cast<float>(scale);
            auto around = region.inflated(static_cast<int>(Math::ceil(sigma * 3)) + reduction_for(sigma) * 2);
            for (auto const& rect : rects) {
                if (around.intersects(rect)) {
                    invalidate(around);
                    break;
                }
            }
        }
    }
}

Vector<IntRect> Compositor::pieces_of(Vector<IntRect> const& rects) const
{
    Vector<IntRect> pieces;
    for (auto const& rect : rects) {
        // On a fixed grid of bands, whatever the rectangle.
        for (int y = rect.y; y < rect.bottom();) {
            int next = min((y / band_height + 1) * band_height, rect.bottom());
            pieces.append(IntRect { rect.x, y, rect.width, next - y });
            y = next;
        }
    }
    return pieces;
}

void Compositor::draw_layers(Layer const* layers, size_t count, Vector<IntRect> const& pieces)
{
    if (count == 0 || pieces.is_empty())
        return;
    LayerJob work { layers, count, &pieces, this };
    m_workers.run(pieces.size(), [](void* context, size_t index) {
        auto const& job = *static_cast<LayerJob*>(context);
        auto const& piece = (*job.pieces)[index];
        for (size_t i = 0; i < job.layer_count; ++i) {
            auto const& layer = job.layers[i];
            switch (layer.kind) {
            case LayerKind::Wallpaper:
                job.compositor->draw_wallpaper(piece);
                break;
            case LayerKind::Shadow:
                job.compositor->draw_shadow(*layer.window, piece);
                break;
            case LayerKind::Body:
                job.compositor->draw_body(*layer.window, piece);
                break;
            }
        }
    },
        &work);
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

void Compositor::draw_body(ServerWindow const& window, IntRect clip)
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
    for (auto& pixel : fill)
        pixel = background;
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

void Compositor::render_material(MaterialStyle const& style, IntRect area, Bitmap& result)
{
    auto& framebuffer = m_screen.framebuffer();
    float sigma = style.blur * static_cast<float>(m_screen.scale());
    int reduction = reduction_for(sigma);
    int reach = static_cast<int>(Math::ceil(sigma * 3)) + reduction * 2;

    // What to read, on a grid of the reduction, so the same pixels come out
    // whichever part is being redrawn: nothing shimmers as things move.
    auto source = area.inflated(reach).intersected(framebuffer.rect());
    int grid_left = source.x / reduction * reduction;
    int grid_top = source.y / reduction * reduction;
    int small_width = (source.right() - grid_left + reduction - 1) / reduction;
    int small_height = (source.bottom() - grid_top + reduction - 1) / reduction;
    auto small_or_error = Bitmap::create({ small_width, small_height });
    if (small_or_error.is_error())
        return;
    auto small = small_or_error.release_value();

    // Down: each small pixel the average of what it covers (of every other
    // pixel, at the widest blurs, which leave no trace of the difference).
    int stride = reduction >= 8 ? 2 : 1;
    int per_side = reduction / stride;
    u32 reciprocal = 65536u / static_cast<u32>(per_side * per_side);
    for (int y = 0; y < small_height; ++y) {
        auto* out = small->scanline(y);
        for (int x = 0; x < small_width; ++x) {
            u32x4 sum {};
            for (int sy = 0; sy < per_side; ++sy) {
                auto const* row = framebuffer.scanline(min(grid_top + y * reduction + sy * stride, framebuffer.height() - 1));
                for (int sx = 0; sx < per_side; ++sx)
                    sum += widened(row[min(grid_left + x * reduction + sx * stride, framebuffer.width() - 1)]);
            }
            out[x] = narrowed((sum * reciprocal + 32768u) >> 16);
        }
    }
    if (gaussian_blur(*small, small->rect(), sigma / static_cast<float>(reduction)).is_error())
        return;
    adjust_saturation(*small, small->rect(), style.saturation);

    // Back up: bilinear (a row between two small rows, then across it), then the
    // tint. Into the screen comes later, once all of them are done: they read it.
    Pixel tint = style.tint.to_pixel();
    Vector<Pixel> between;
    MUST(between.try_resize(static_cast<size_t>(small_width)));
    int half = reduction / 2;
    u32 step = 256u / static_cast<u32>(reduction);
    for (int y = area.y; y < area.bottom(); ++y) {
        int position_y = max(y - grid_top - half, 0);
        int upper = min(position_y / reduction, small_height - 1);
        int lower = min(upper + 1, small_height - 1);
        u32 fraction_y = static_cast<u32>(position_y % reduction) * step;
        auto const* top = small->scanline(upper);
        auto const* bottom = small->scanline(lower);
        for (int x = 0; x < small_width; ++x)
            between[static_cast<size_t>(x)] = narrowed((widened(top[x]) * (256 - fraction_y) + widened(bottom[x]) * fraction_y + 128u) >> 8);

        auto* row_buffer = result.scanline(y - area.y);
        int position_x = max(area.x - grid_left - half, 0);
        int offset = position_x % reduction;
        int column = position_x / reduction;
        int written = 0;
        while (written < area.width) {
            u32x4 left = widened(between[static_cast<size_t>(min(column, small_width - 1))]);
            u32x4 right = widened(between[static_cast<size_t>(min(column + 1, small_width - 1))]);
            int run = min(reduction - offset, area.width - written);
            for (int i = 0; i < run; ++i) {
                u32 fraction_x = static_cast<u32>(offset + i) * step;
                row_buffer[written + i] = narrowed((left * (256 - fraction_x) + right * fraction_x + 128u) >> 8);
            }
            written += run;
            offset = 0;
            ++column;
        }
        blend_row_solid(row_buffer, tint, area.width);
    }
}

void Compositor::place_material(ServerWindow const& window, IntRect area, Bitmap const& result)
{
    auto& framebuffer = m_screen.framebuffer();
    auto frame = device_frame(window);
    for (int y = area.y; y < area.bottom(); ++y)
        blend_row_in_window(framebuffer.scanline(y) + area.x, result.scanline(y - area.y), area.x - frame.x, y - frame.y, area.width, frame.size(), window.is_bare() ? nullptr : window.shape);
}

void Compositor::render()
{
    if (!m_any_dirty)
        return;
    u64 started = now_microseconds();
    grow_damage_for_materials();
    auto rects = dirty_rects();
    auto pieces = pieces_of(rects);

    // Bottom to top, stopping where a material needs what is below it done.
    Vector<Layer> layers;
    layers.append(Layer { LayerKind::Wallpaper, nullptr });
    int scale = m_screen.scale();
    struct MaterialJob {
        Compositor* compositor;
        ServerWindow const* window;
        MaterialStyle const* style;
        IntRect area;
        RefPtr<Bitmap> result;
    };
    for (auto* window : m_windows) {
        if (!window->visible)
            continue;
        layers.append(Layer { LayerKind::Shadow, window });
        if (!window->materials.is_empty()) {
            draw_layers(layers.data(), layers.size(), pieces);
            layers.clear();
            auto frame = device_frame(*window);
            Vector<MaterialJob> jobs;
            for (auto const& material : window->materials) {
                IntRect region { frame.x + material.rect.x * scale, frame.y + material.rect.y * scale, material.rect.width * scale, material.rect.height * scale };
                region = region.intersected(frame);
                auto const& style = m_theme.materials[static_cast<int>(material.material)];
                for (auto const& rect : rects) {
                    auto area = region.intersected(rect);
                    if (area.is_empty())
                        continue;
                    auto result = Bitmap::create(area.size());
                    if (!result.is_error())
                        jobs.append(MaterialJob { this, window, &style, area, result.release_value() });
                }
            }
            // All from what is below, none reading what another wrote, whichever
            // thread gets there first; and only then into the screen.
            m_workers.run(jobs.size(), [](void* context, size_t index) {
                auto const& job = static_cast<MaterialJob const*>(context)[index];
                job.compositor->render_material(*job.style, job.area, *job.result);
            },
                jobs.data());
            m_workers.run(jobs.size(), [](void* context, size_t index) {
                auto const& job = static_cast<MaterialJob const*>(context)[index];
                job.compositor->place_material(*job.window, job.area, *job.result);
            },
                jobs.data());
        }
        layers.append(Layer { LayerKind::Body, window });
    }
    draw_layers(layers.data(), layers.size(), pieces);
    u64 composed = now_microseconds();

    // One flush for each part; far apart parts stay apart.
    u64 pixel_count = 0;
    if (rects.size() > 16) {
        IntRect everything;
        for (auto const& rect : rects)
            everything = everything.united(rect);
        m_screen.flush(everything);
        pixel_count = static_cast<u64>(everything.width) * static_cast<u64>(everything.height);
    } else {
        for (auto const& rect : rects) {
            m_screen.flush(rect);
            pixel_count += static_cast<u64>(rect.width) * static_cast<u64>(rect.height);
        }
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
