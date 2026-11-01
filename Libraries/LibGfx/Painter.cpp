/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibBase/New.h>
#include <LibGfx/Blend.h>
#include <LibGfx/Painter.h>

namespace Gfx {

namespace Math = Base::Math;

namespace {

bool is_whole(float value)
{
    return Math::floor(value) == value;
}

// Colors from a gradient, in device coordinates.
struct GradientShader {
    Gradient const* gradient;
    AffineTransform inverse;
    // Linear: t = t_at_origin + x dt_dx + y dt_dy.
    float t_at_origin;
    float dt_dx;
    float dt_dy;
};

void shade_linear(void* context, int y, int x, int count, Pixel* row)
{
    auto const& shader = *static_cast<GradientShader*>(context);
    float t = shader.t_at_origin + (static_cast<float>(x) + 0.5f) * shader.dt_dx + (static_cast<float>(y) + 0.5f) * shader.dt_dy;
    for (int i = 0; i < count; ++i)
        row[i] = shader.gradient->pixel_at(t + static_cast<float>(i) * shader.dt_dx);
}

void shade_radial(void* context, int y, int x, int count, Pixel* row)
{
    auto const& shader = *static_cast<GradientShader*>(context);
    auto center = shader.gradient->from();
    float radius = shader.gradient->radius();
    for (int i = 0; i < count; ++i) {
        auto point = shader.inverse.map(FloatPoint { static_cast<float>(x + i) + 0.5f, static_cast<float>(y) + 0.5f });
        float dx = point.x - center.x;
        float dy = point.y - center.y;
        row[i] = shader.gradient->pixel_at(Math::square_root(dx * dx + dy * dy) / radius);
    }
}

// A bitmap's pixels through a transform.
struct BitmapShader {
    Bitmap const* bitmap;
    // Device coordinates to source pixels.
    AffineTransform to_source;
    u32 opacity;
    // Sampling stays inside this.
    IntRect limit;
};

void shade_bitmap(void* context, int y, int x, int count, Pixel* row)
{
    auto const& shader = *static_cast<BitmapShader*>(context);
    auto const& limit = shader.limit;
    for (int i = 0; i < count; ++i) {
        auto point = shader.to_source.map(FloatPoint { static_cast<float>(x + i) + 0.5f, static_cast<float>(y) + 0.5f });
        // Between the four nearest pixel centers.
        float u = point.x - 0.5f;
        float v = point.y - 0.5f;
        float floor_u = Math::floor(u);
        float floor_v = Math::floor(v);
        u32 fraction_u = static_cast<u32>((u - floor_u) * 256.0f);
        u32 fraction_v = static_cast<u32>((v - floor_v) * 256.0f);
        int left = clamp(static_cast<int>(floor_u), limit.x, limit.right() - 1);
        int right = clamp(static_cast<int>(floor_u) + 1, limit.x, limit.right() - 1);
        int top = clamp(static_cast<int>(floor_v), limit.y, limit.bottom() - 1);
        int bottom = clamp(static_cast<int>(floor_v) + 1, limit.y, limit.bottom() - 1);
        auto const* upper = shader.bitmap->scanline(top);
        auto const* lower = shader.bitmap->scanline(bottom);
        Pixel a = upper[left], b = upper[right], c = lower[left], d = lower[right];
        auto channel = [&](auto get) {
            u32 top_mix = get(a) * (256 - fraction_u) + get(b) * fraction_u;
            u32 bottom_mix = get(c) * (256 - fraction_u) + get(d) * fraction_u;
            return static_cast<u8>((top_mix * (256 - fraction_v) + bottom_mix * fraction_v + 32768) >> 16);
        };
        Pixel sampled = make_pixel(channel([](Pixel p) { return static_cast<u32>(pixel_red(p)); }), channel([](Pixel p) { return static_cast<u32>(pixel_green(p)); }),
            channel([](Pixel p) { return static_cast<u32>(pixel_blue(p)); }), channel([](Pixel p) { return static_cast<u32>(pixel_alpha(p)); }));
        row[i] = shader.opacity >= 255 ? sampled : scale_pixel(sampled, shader.opacity);
    }
}

}

Painter::Painter(Bitmap& target)
    : m_target(target)
{
    m_state.clip = target.rect();
}

void Painter::save()
{
    m_stack.append(m_state);
}

void Painter::restore()
{
    VERIFY(!m_stack.is_empty());
    m_state = m_stack.take_last();
}

void Painter::translate(float x, float y)
{
    concat(AffineTransform::translation(x, y));
}

void Painter::scale(float x, float y)
{
    concat(AffineTransform::scaling(x, y));
}

void Painter::rotate(float radians)
{
    concat(AffineTransform::rotation(radians));
}

void Painter::concat(AffineTransform const& transform)
{
    m_state.transform = m_state.transform.multiplied(transform);
}

void Painter::clip_rect(FloatRect const& rect)
{
    auto const& transform = m_state.transform;
    if (transform.is_axis_aligned()) {
        auto device = transform.map(rect);
        if (is_whole(device.x) && is_whole(device.y) && is_whole(device.right()) && is_whole(device.bottom())) {
            m_state.clip = m_state.clip.intersected(device.to<int>());
            return;
        }
    }
    Path path;
    path.add_rect(rect);
    clip_path(path);
}

void Painter::clip_path(Path const& path, FillRule rule)
{
    auto device_path = path.transformed(m_state.transform);
    auto area = m_state.clip.intersected(device_path.bounds().enclosing_int_rect());
    auto mask = adopt_ref(*new ClipMask);
    mask->area = area;
    MUST(mask->coverage.try_resize(static_cast<size_t>(max(area.width, 0)) * static_cast<size_t>(max(area.height, 0))));
    if (!mask->coverage.is_empty())
        __builtin_memset(mask->coverage.data(), 0, mask->coverage.size());
    if (!area.is_empty()) {
        Rasterizer rasterizer(area);
        rasterizer.add_path(device_path);
        auto const* previous = m_state.mask.ptr();
        rasterizer.rasterize(rule, [&](int y, int x, int count, u8 const* coverage) {
            auto* row = const_cast<u8*>(mask->row(y)) + (x - area.x);
            for (int i = 0; i < count; ++i) {
                u32 value = coverage[i];
                if (previous) {
                    int px = x + i;
                    bool inside = px >= previous->area.x && px < previous->area.right() && y >= previous->area.y && y < previous->area.bottom();
                    value = inside ? divide_by_255(value * previous->row(y)[px - previous->area.x]) : 0;
                }
                row[i] = static_cast<u8>(value);
            }
        });
    }
    m_state.clip = area;
    m_state.mask = move(mask);
}

u8 const* Painter::masked_coverage(int y, int x, int count, u8 const* coverage)
{
    auto const& mask = *m_state.mask;
    MUST(m_coverage_row.try_resize(static_cast<size_t>(count)));
    auto* out = m_coverage_row.data();
    auto const* mask_row = mask.row(y) + (x - mask.area.x);
    if (coverage) {
        for (int i = 0; i < count; ++i)
            out[i] = static_cast<u8>(divide_by_255(static_cast<u32>(coverage[i]) * mask_row[i]));
    } else {
        for (int i = 0; i < count; ++i)
            out[i] = mask_row[i];
    }
    return out;
}

void Painter::fill_device_path(Path const& device_path, FillRule rule, Pixel solid, Shader shader, void* context)
{
    auto area = m_state.clip.intersected(device_path.bounds().enclosing_int_rect());
    if (area.is_empty())
        return;
    if (shader)
        MUST(m_shader_row.try_resize(static_cast<size_t>(area.width)));
    Rasterizer rasterizer(area);
    rasterizer.add_path(device_path);
    rasterizer.rasterize(rule, [&](int y, int x, int count, u8 const* coverage) {
        if (m_state.mask)
            coverage = masked_coverage(y, x, count, coverage);
        auto* destination = m_target.scanline(y) + x;
        if (shader) {
            shader(context, y, x, count, m_shader_row.data());
            blend_row_coverage(destination, m_shader_row.data(), coverage, count);
        } else {
            blend_row_solid_coverage(destination, solid, coverage, count);
        }
    });
}

void Painter::fill_rect(FloatRect const& rect, Color color)
{
    auto const& transform = m_state.transform;
    if (transform.is_axis_aligned() && !m_state.mask) {
        auto device = transform.map(rect);
        if (is_whole(device.x) && is_whole(device.y) && is_whole(device.right()) && is_whole(device.bottom())) {
            auto area = m_state.clip.intersected(device.to<int>());
            Pixel value = color.to_pixel();
            for (int y = area.y; y < area.bottom(); ++y)
                blend_row_solid(m_target.scanline(y) + area.x, value, area.width);
            return;
        }
    }
    Path path;
    path.add_rect(rect);
    fill_path(path, color);
}

void Painter::fill_path(Path const& path, Color color, FillRule rule)
{
    fill_device_path(path.transformed(m_state.transform), rule, color.to_pixel(), nullptr, nullptr);
}

void Painter::fill_with_gradient(Path const& device_path, Gradient const& gradient, FillRule rule)
{
    auto const& transform = m_state.transform;
    if (!transform.is_invertible())
        return;
    GradientShader shader { &gradient, transform.inverse(), 0, 0, 0 };
    if (gradient.is_radial()) {
        fill_device_path(device_path, rule, 0, shade_radial, &shader);
        return;
    }
    // t is affine in user space, so also in device space.
    FloatPoint along = gradient.to() - gradient.from();
    float length_squared = along.x * along.x + along.y * along.y;
    if (length_squared == 0) {
        fill_device_path(device_path, rule, gradient.pixel_at(1), nullptr, nullptr);
        return;
    }
    auto t_at = [&](FloatPoint device) {
        auto user = shader.inverse.map(device);
        return ((user.x - gradient.from().x) * along.x + (user.y - gradient.from().y) * along.y) / length_squared;
    };
    shader.t_at_origin = t_at({ 0, 0 });
    shader.dt_dx = t_at({ 1, 0 }) - shader.t_at_origin;
    shader.dt_dy = t_at({ 0, 1 }) - shader.t_at_origin;
    fill_device_path(device_path, rule, 0, shade_linear, &shader);
}

void Painter::fill_path(Path const& path, Gradient const& gradient, FillRule rule)
{
    fill_with_gradient(path.transformed(m_state.transform), gradient, rule);
}

void Painter::stroke_path(Path const& path, StrokeStyle const& style, Color color)
{
    float scale = max(m_state.transform.maximum_scale(), 0.001f);
    fill_path(stroked(path, style, 0.025f / scale), color);
}

void Painter::stroke_path(Path const& path, StrokeStyle const& style, Gradient const& gradient)
{
    float scale = max(m_state.transform.maximum_scale(), 0.001f);
    fill_path(stroked(path, style, 0.025f / scale), gradient);
}

void Painter::fill_rounded_rect(FloatRect const& rect, float radius, Color color)
{
    Path path;
    path.add_rounded_rect(rect, radius);
    fill_path(path, color);
}

void Painter::fill_continuous_rounded_rect(FloatRect const& rect, float radius, Color color)
{
    Path path;
    path.add_continuous_rounded_rect(rect, radius);
    fill_path(path, color);
}

void Painter::draw_bitmap(Bitmap const& bitmap, FloatRect const& destination, float opacity)
{
    draw_bitmap(bitmap, bitmap.rect().to<float>(), destination, opacity);
}

void Painter::draw_bitmap(Bitmap const& bitmap, FloatRect const& source, FloatRect const& destination, float opacity)
{
    if (source.is_empty() || destination.is_empty())
        return;
    auto const opacity_value = static_cast<u32>(clamp(opacity, 0.0f, 1.0f) * 255.0f + 0.5f);
    auto const& transform = m_state.transform;
    auto device = transform.map(destination);

    // Pixel for pixel: no resampling at all.
    if (transform.is_axis_aligned() && !m_state.mask && device.width == source.width && device.height == source.height && is_whole(device.x)
        && is_whole(device.y) && is_whole(source.x) && is_whole(source.y) && is_whole(source.width) && is_whole(source.height)) {
        blit(bitmap, device.origin().to<int>(), source.to<int>(), opacity_value);
        return;
    }

    // Shrinking a lot: start from an averaged copy, so nothing is skipped.
    float shrink = min(device.width / source.width, device.height / source.height);
    if (shrink <= 0.5f) {
        auto smaller = bitmap.halved();
        if (!smaller.is_error()) {
            FloatRect half_source { source.x / 2, source.y / 2, source.width / 2, source.height / 2 };
            draw_bitmap(*smaller.value(), half_source, destination, opacity);
            return;
        }
    }

    // destination -> source, after device -> destination.
    auto from_destination = AffineTransform(source.width / destination.width, 0, 0, source.height / destination.height,
        source.x - destination.x * source.width / destination.width, source.y - destination.y * source.height / destination.height);
    if (!transform.is_invertible())
        return;
    BitmapShader shader { &bitmap, from_destination.multiplied(transform.inverse()), opacity_value, source.enclosing_int_rect().intersected(bitmap.rect()) };
    if (shader.limit.is_empty())
        return;
    Path path;
    path.add_rect(destination);
    fill_device_path(path.transformed(transform), FillRule::NonZero, 0, shade_bitmap, &shader);
}

void Painter::blit(Bitmap const& bitmap, IntPoint position, IntRect source, u32 opacity)
{
    source = source.intersected(bitmap.rect());
    IntRect destination { position.x, position.y, source.width, source.height };
    auto area = destination.intersected(m_state.clip).intersected(m_target.rect());
    for (int y = area.y; y < area.bottom(); ++y) {
        auto const* from = bitmap.scanline(source.y + y - destination.y) + source.x + (area.x - destination.x);
        auto* to = m_target.scanline(y) + area.x;
        if (m_state.mask) {
            MUST(m_shader_row.try_resize(static_cast<size_t>(area.width)));
            if (opacity < 255) {
                for (int x = 0; x < area.width; ++x)
                    m_shader_row[static_cast<size_t>(x)] = scale_pixel(from[x], opacity);
                from = m_shader_row.data();
            }
            blend_row_coverage(to, from, masked_coverage(y, area.x, area.width, nullptr), area.width);
        } else {
            blend_row_opacity(to, from, opacity, area.width);
        }
    }
}

void Painter::draw_mask(AlphaMask const& mask, IntPoint position, Color color)
{
    draw_mask(mask.scanline(0), mask.width(), { position.x, position.y, mask.width(), mask.height() }, color);
}

void Painter::draw_mask(u8 const* values, int pitch, IntRect destination, Color color)
{
    auto area = destination.intersected(m_state.clip).intersected(m_target.rect());
    Pixel value = color.to_pixel();
    for (int y = area.y; y < area.bottom(); ++y) {
        u8 const* coverage = values + static_cast<ptrdiff_t>(y - destination.y) * pitch + (area.x - destination.x);
        if (m_state.mask)
            coverage = masked_coverage(y, area.x, area.width, coverage);
        blend_row_solid_coverage(m_target.scanline(y) + area.x, value, coverage, area.width);
    }
}

}
