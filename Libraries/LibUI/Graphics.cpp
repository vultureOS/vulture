/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibBase/String.h>
#include <LibUI/Graphics.h>

namespace UI {

namespace Math = Base::Math;
using namespace Gfx;

namespace {

struct TokenName {
    char buffer[96];
    size_t length { 0 };

    TokenName(StringView group, StringView name)
    {
        add(group);
        add("."_sv);
        add(name);
    }

    void add(StringView part)
    {
        for (size_t i = 0; i < part.length() && length < sizeof(buffer); ++i)
            buffer[length++] = part[i];
    }

    [[nodiscard]] StringView view() const { return StringView(buffer, length); }
};

// A rectangle with its corners rounded, whichever way is measured to look right.
void add_shape(Path& path, FloatRect const& rect, float radius)
{
    float largest = min(rect.width, rect.height) / 2;
    path.add_rounded_rect(rect, clamp(radius, 0.0f, largest));
}

void fill_layers(Painter& painter, FloatRect const& rect, float radius, Span<float const> layers, Color color)
{
    for (size_t i = 0; i + 3 < layers.size(); i += 4) {
        float top = layers[i], bottom = layers[i + 1], side = layers[i + 2], alpha = layers[i + 3];
        FloatRect grown { rect.x - side, rect.y - top, rect.width + side * 2, rect.height + top + bottom };
        // Only outside the shape: a translucent fill must not show them through it.
        Path path;
        add_shape(path, grown, radius + (side + top) / 2);
        add_shape(path, rect, radius);
        painter.fill_path(path, color.with_alpha(static_cast<u8>(alpha * 255 + 0.5f)), FillRule::EvenOdd);
    }
}

}

BezelPaint BezelPaint::from_tokens(StringView group)
{
    auto const& appearance = Appearance::the();
    BezelPaint paint;
    TokenName fill(group, "fill"_sv);
    if (appearance.has_color(fill.view())) {
        paint.top = paint.bottom = appearance.color(fill.view());
    } else {
        paint.top = appearance.color(TokenName(group, "fillTop"_sv).view());
        paint.bottom = appearance.color(TokenName(group, "fillBottom"_sv).view());
    }
    paint.outer = appearance.list(TokenName(group, "outer"_sv).view());
    paint.inner = appearance.list(TokenName(group, "inner"_sv).view());
    TokenName inner_white(group, "innerWhite"_sv);
    if (appearance.has_list(inner_white.view()))
        paint.inner_white = appearance.list(inner_white.view());
    TokenName outer_color(group, "outerColor"_sv);
    if (appearance.has_color(outer_color.view()))
        paint.outer_color = appearance.color(outer_color.view());
    TokenName highlight(group, "highlight"_sv);
    if (appearance.has_color(highlight.view()))
        paint.highlight = appearance.color(highlight.view());
    else
        paint.highlight = { 0, 0, 0, 0 };
    paint.soft = appearance.metric(TokenName(group, "soft"_sv).view());
    return paint;
}

Graphics::Graphics(Painter& painter, float scale)
    : m_painter(painter)
    , m_scale(scale)
{
}

float Graphics::snap(float position) const
{
    // Where it lands on the device, to the nearest pixel, and back.
    auto const& transform = m_painter.transform();
    float offset = transform.e();
    float device = position * transform.a() + offset;
    return (static_cast<float>(Math::round(static_cast<double>(device))) - offset) / transform.a();
}

FloatRect Graphics::snapped(FloatRect const& rect) const
{
    auto const& transform = m_painter.transform();
    auto round_x = [&](float x) { return (static_cast<float>(Math::round(static_cast<double>(x * transform.a() + transform.e()))) - transform.e()) / transform.a(); };
    auto round_y = [&](float y) { return (static_cast<float>(Math::round(static_cast<double>(y * transform.d() + transform.f()))) - transform.f()) / transform.d(); };
    return FloatRect::from_edges(round_x(rect.left()), round_y(rect.top()), round_x(rect.right()), round_y(rect.bottom()));
}

void Graphics::fill(FloatRect const& rect, Color color)
{
    m_painter.fill_rect(rect, color);
}

void Graphics::fill_rounded(FloatRect const& rect, float radius, Color color)
{
    m_painter.fill_rounded_rect(rect, radius, color);
}

void Graphics::fill_continuous(FloatRect const& rect, float radius, Color color)
{
    m_painter.fill_continuous_rounded_rect(rect, radius, color);
}

void Graphics::fill_vertical_gradient(FloatRect const& rect, float radius, Color top, Color bottom)
{
    GradientStop stops[2] = { { 0, top }, { 1, bottom } };
    auto gradient = Gradient::linear({ rect.x, rect.y }, { rect.x, rect.bottom() }, Span<GradientStop const>(stops, 2));
    Path path;
    add_shape(path, rect, radius);
    m_painter.fill_path(path, gradient);
}

void Graphics::fill_circle(FloatPoint center, float radius, Color color)
{
    Path path;
    path.add_circle(center, radius);
    m_painter.fill_path(path, color);
}

void Graphics::stroke_rounded(FloatRect const& rect, float radius, float width, Color color)
{
    Path path;
    add_shape(path, rect, radius);
    FloatRect inner { rect.x + width, rect.y + width, rect.width - width * 2, rect.height - width * 2 };
    if (!inner.is_empty())
        add_shape(path, inner, max(radius - width, 0.0f));
    m_painter.fill_path(path, color, FillRule::EvenOdd);
}

void Graphics::line(FloatPoint from, FloatPoint to, float width, Color color, bool round_caps)
{
    Path path;
    path.move_to(from);
    path.line_to(to);
    StrokeStyle style;
    style.width = width;
    if (round_caps)
        style.cap = LineCap::Round;
    m_painter.stroke_path(path, style, color);
}

void Graphics::draw_text(StringView text, FontSpec spec, FloatPoint position, Color color)
{
    if (text.is_empty())
        return;
    auto device = m_painter.transform().map(position);
    device.y = static_cast<float>(Math::round(static_cast<double>(device.y)));
    Appearance::font(spec, m_scale).draw(m_painter, device, text, color);
}

void Graphics::draw_text_centered(StringView text, FontSpec spec, FloatRect const& rect, Color color, TextAlignment alignment)
{
    float width = Appearance::text_width(spec, text);
    float x = rect.x;
    if (alignment == TextAlignment::Center)
        x = rect.x + (rect.width - width) / 2;
    else if (alignment == TextAlignment::Right)
        x = rect.right() - width;
    float baseline = rect.y + (rect.height + Appearance::cap_height(spec)) / 2;
    draw_text(text, spec, { x, baseline }, color);
}

void Graphics::draw_text_truncated(StringView text, FontSpec spec, FloatPoint position, float max_width, Color color)
{
    if (Appearance::text_width(spec, text) <= max_width) {
        draw_text(text, spec, position, color);
        return;
    }
    auto ellipsis = "\xE2\x80\xA6"_sv;
    float ellipsis_width = Appearance::text_width(spec, ellipsis);
    // Whole characters off the end until it fits.
    size_t length = text.length();
    while (length > 0) {
        --length;
        while (length > 0 && (static_cast<u8>(text[length]) & 0xC0) == 0x80)
            --length;
        if (Appearance::text_width(spec, text.substring_view(0, length)) + ellipsis_width <= max_width)
            break;
    }
    StringBuilder builder;
    builder.append(text.substring_view(0, length));
    builder.append(ellipsis);
    draw_text(builder.view(), spec, position, color);
}

void Graphics::draw_bezel(FloatRect const& rect, float radius, BezelPaint const& paint)
{
    fill_layers(m_painter, rect, radius, paint.outer, paint.outer_color);

    float soft = m_scale >= 2 ? paint.soft : 0;
    if (soft > 0) {
        float one = 1 / m_scale;
        // A pixel of it outside, the rest of the first row inside: as if drawn at a
        // higher resolution with a slight blur.
        FloatRect outside = rect.inflated(one);
        fill_rounded(outside, radius + one, paint.top.with_opacity(soft));
        // Over that, the edge row comes to 1 - soft; the rows inside, whole.
        float a = static_cast<float>(paint.top.alpha) / 255;
        float have = soft * a;
        float want = (1 - soft) * a;
        float factor = have >= 1 ? 0 : (want - have) / (1 - have) / max(a, 0.001f);
        if (paint.top == paint.bottom) {
            fill_rounded(rect, radius, paint.top.with_opacity(factor));
        } else {
            fill_vertical_gradient(rect, radius, paint.top.with_opacity(factor), paint.bottom.with_opacity(factor));
        }
        FloatRect inside { rect.x + one, rect.y + one, rect.width - one * 2, rect.height - one * 2 };
        float reached = 1 - (1 - have) * (1 - factor * a);
        float last = reached >= 1 ? 0 : (a - reached) / (1 - reached) / max(a, 0.001f);
        if (paint.top == paint.bottom)
            fill_rounded(inside, max(radius - one, 0.0f), paint.top.with_opacity(last));
        else
            fill_vertical_gradient(inside, max(radius - one, 0.0f), paint.top.with_opacity(last), paint.bottom.with_opacity(last));
    } else if (paint.top == paint.bottom) {
        fill_rounded(rect, radius, paint.top);
    } else {
        fill_vertical_gradient(rect, radius, paint.top, paint.bottom);
    }

    // Rings inside: from the edge of the shape in, by that much on each side.
    auto inner_rings = [&](Span<float const> rings, u8 shade) {
        for (size_t i = 0; i + 3 < rings.size(); i += 4) {
            float top = rings[i], bottom = rings[i + 1], side = rings[i + 2], alpha = rings[i + 3];
            Path path;
            add_shape(path, rect, radius);
            FloatRect hole { rect.x + side, rect.y + top, rect.width - side * 2, rect.height - top - bottom };
            if (!hole.is_empty())
                add_shape(path, hole, max(radius - (side + top) / 2, 0.0f));
            m_painter.fill_path(path, Color { shade, shade, shade, static_cast<u8>(alpha * 255 + 0.5f) }, FillRule::EvenOdd);
        }
    };
    inner_rings(paint.inner, 0);
    inner_rings(paint.inner_white, 255);

    if (paint.highlight.alpha > 0) {
        // A point of light along the top, inside the shape.
        save();
        Path shape;
        add_shape(shape, rect, radius);
        m_painter.clip_path(shape);
        fill({ rect.x, rect.y - pixel(), rect.width, 1 }, paint.highlight);
        restore();
    }
}

void Graphics::draw_symbol(Symbol symbol, FloatRect const& rect, Color color, float weight)
{
    // Drawn on a grid of 16 by 16 fitted to the rectangle: original shapes, strokes
    // with round ends, as UI glyphs should be at these sizes.
    float unit = min(rect.width, rect.height) / 16;
    float ox = rect.x + (rect.width - unit * 16) / 2;
    float oy = rect.y + (rect.height - unit * 16) / 2;
    auto at = [&](float x, float y) { return FloatPoint { ox + x * unit, oy + y * unit }; };
    StrokeStyle style;
    style.width = (weight > 0 ? weight : 1.6f) * unit;
    style.cap = LineCap::Round;
    style.join = LineJoin::Round;
    Path path;
    switch (symbol) {
    case Symbol::Checkmark:
        path.move_to(at(3.2f, 8.6f));
        path.line_to(at(6.6f, 12.2f));
        path.line_to(at(12.8f, 3.8f));
        break;
    case Symbol::Dash:
        path.move_to(at(3.6f, 8));
        path.line_to(at(12.4f, 8));
        break;
    case Symbol::ChevronUpDown:
        path.move_to(at(5, 6.2f));
        path.line_to(at(8, 3.2f));
        path.line_to(at(11, 6.2f));
        path.move_to(at(5, 9.8f));
        path.line_to(at(8, 12.8f));
        path.line_to(at(11, 9.8f));
        break;
    case Symbol::ChevronDown:
        path.move_to(at(3.6f, 6));
        path.line_to(at(8, 10.4f));
        path.line_to(at(12.4f, 6));
        break;
    case Symbol::ChevronUp:
        path.move_to(at(3.6f, 10));
        path.line_to(at(8, 5.6f));
        path.line_to(at(12.4f, 10));
        break;
    case Symbol::ChevronRight:
        path.move_to(at(6, 3.6f));
        path.line_to(at(10.4f, 8));
        path.line_to(at(6, 12.4f));
        break;
    case Symbol::ChevronLeft:
        path.move_to(at(10, 3.6f));
        path.line_to(at(5.6f, 8));
        path.line_to(at(10, 12.4f));
        break;
    case Symbol::Disclosure: {
        Path triangle;
        triangle.move_to(at(5, 3));
        triangle.line_to(at(12, 8));
        triangle.line_to(at(5, 13));
        triangle.close();
        m_painter.fill_path(triangle, color);
        return;
    }
    case Symbol::Plus:
        path.move_to(at(8, 3));
        path.line_to(at(8, 13));
        path.move_to(at(3, 8));
        path.line_to(at(13, 8));
        break;
    case Symbol::Minus:
        path.move_to(at(3, 8));
        path.line_to(at(13, 8));
        break;
    case Symbol::Magnifier:
        path.arc_to(at(7, 7), 4.4f * unit, 0, 2 * static_cast<float>(Math::pi));
        path.move_to(at(10.4f, 10.4f));
        path.line_to(at(13.6f, 13.6f));
        break;
    case Symbol::ClearCircle: {
        Path disc;
        disc.add_circle(at(8, 8), 6.5f * unit);
        // The cross is cut out of the disc.
        float half = 0.75f * unit;
        auto bar = [&](FloatPoint a, FloatPoint b, FloatPoint c, FloatPoint d) {
            disc.move_to(a);
            disc.line_to(b);
            disc.line_to(c);
            disc.line_to(d);
            disc.close();
        };
        FloatPoint p0 = at(5.2f, 5.2f), p1 = at(10.8f, 10.8f), p2 = at(10.8f, 5.2f), p3 = at(5.2f, 10.8f);
        bar({ p0.x - half, p0.y + half }, { p0.x + half, p0.y - half }, { p1.x + half, p1.y - half }, { p1.x - half, p1.y + half });
        bar({ p2.x - half, p2.y - half }, { p2.x + half, p2.y + half }, { p3.x + half, p3.y + half }, { p3.x - half, p3.y - half });
        m_painter.fill_path(disc, color, FillRule::EvenOdd);
        return;
    }
    case Symbol::Question: {
        path.arc_to(at(8, 6), 2.8f * unit, static_cast<float>(Math::pi), 2.25f * static_cast<float>(Math::pi));
        path.line_to(at(8, 9.8f));
        m_painter.stroke_path(path, style, color);
        fill_circle(at(8, 12.6f), style.width * 0.62f, color);
        return;
    }
    case Symbol::Home: {
        Path house;
        float const points[][2] = { { 8, 2.4f }, { 14.4f, 8.2f }, { 12.6f, 8.2f }, { 12.6f, 13.6f }, { 9.6f, 13.6f }, { 9.6f, 9.8f }, { 6.4f, 9.8f }, { 6.4f, 13.6f }, { 3.4f, 13.6f }, { 3.4f, 8.2f }, { 1.6f, 8.2f } };
        house.move_to(at(points[0][0], points[0][1]));
        for (size_t i = 1; i < array_size(points); ++i)
            house.line_to(at(points[i][0], points[i][1]));
        house.close();
        m_painter.fill_path(house, color);
        return;
    }
    case Symbol::Desktop: {
        m_painter.fill_rounded_rect({ at(1.6f, 2.6f).x, at(1.6f, 2.6f).y, 12.8f * unit, 8.6f * unit }, 1.2f * unit, color);
        path.move_to(at(8, 11));
        path.line_to(at(8, 13.2f));
        path.move_to(at(5.4f, 13.4f));
        path.line_to(at(10.6f, 13.4f));
        break;
    }
    case Symbol::Download: {
        path.move_to(at(8, 2.4f));
        path.line_to(at(8, 10.2f));
        path.move_to(at(4.6f, 6.8f));
        path.line_to(at(8, 10.2f));
        path.line_to(at(11.4f, 6.8f));
        path.move_to(at(3, 13.4f));
        path.line_to(at(13, 13.4f));
        break;
    }
    case Symbol::Applications: {
        float const places[2] = { 2.2f, 8.8f };
        for (float y : places) {
            for (float x : places)
                m_painter.fill_rounded_rect({ at(x, y).x, at(x, y).y, 5 * unit, 5 * unit }, 1.2f * unit, color);
        }
        return;
    }
    case Symbol::Trash: {
        Path can;
        can.move_to(at(3.8f, 5.4f));
        can.line_to(at(12.2f, 5.4f));
        can.line_to(at(11.3f, 14.2f));
        can.line_to(at(4.7f, 14.2f));
        can.close();
        m_painter.fill_path(can, color);
        path.move_to(at(2.8f, 3.9f));
        path.line_to(at(13.2f, 3.9f));
        path.move_to(at(6.4f, 3.6f));
        path.line_to(at(6.8f, 2.2f));
        path.line_to(at(9.2f, 2.2f));
        path.line_to(at(9.6f, 3.6f));
        style.width = 1.2f * unit;
        break;
    }
    case Symbol::Drive: {
        Path drive;
        auto corner = at(1.4f, 4.6f);
        drive.add_rounded_rect({ corner.x, corner.y, 13.2f * unit, 6.8f * unit }, 1.6f * unit);
        drive.add_circle(at(11.6f, 8), 0.95f * unit);
        m_painter.fill_path(drive, color, FillRule::EvenOdd);
        return;
    }
    case Symbol::Eject: {
        Path triangle;
        triangle.move_to(at(8, 2.6f));
        triangle.line_to(at(14, 9.2f));
        triangle.line_to(at(2, 9.2f));
        triangle.close();
        auto const corner = at(2, 11);
        triangle.add_rounded_rect({ corner.x, corner.y, 12 * unit, 2.2f * unit }, 0.6f * unit);
        m_painter.fill_path(triangle, color);
        return;
    }
    case Symbol::FirstAid: {
        // A rounded square with a cross cut out of it.
        Path shape;
        auto const corner = at(1.6f, 1.6f);
        shape.add_rounded_rect({ corner.x, corner.y, 12.8f * unit, 12.8f * unit }, 3 * unit);
        shape.move_to(at(6.8f, 4));
        shape.line_to(at(9.2f, 4));
        shape.line_to(at(9.2f, 6.8f));
        shape.line_to(at(12, 6.8f));
        shape.line_to(at(12, 9.2f));
        shape.line_to(at(9.2f, 9.2f));
        shape.line_to(at(9.2f, 12));
        shape.line_to(at(6.8f, 12));
        shape.line_to(at(6.8f, 9.2f));
        shape.line_to(at(4, 9.2f));
        shape.line_to(at(4, 6.8f));
        shape.line_to(at(6.8f, 6.8f));
        shape.close();
        m_painter.fill_path(shape, color, FillRule::EvenOdd);
        return;
    }
    case Symbol::Erase: {
        // An eraser, at an angle, and the line it's going over.
        Path shape;
        shape.move_to(at(1.7f, 8.6f));
        shape.line_to(at(6.3f, 4));
        shape.line_to(at(10.8f, 8.5f));
        shape.line_to(at(6.2f, 13.1f));
        shape.close();
        shape.move_to(at(7.2f, 3.1f));
        shape.line_to(at(9.3f, 1));
        shape.line_to(at(13.8f, 5.5f));
        shape.line_to(at(11.7f, 7.6f));
        shape.close();
        auto const line = at(1.6f, 14);
        shape.add_rounded_rect({ line.x, line.y, 12.8f * unit, 1.2f * unit }, 0.6f * unit);
        m_painter.fill_path(shape, color);
        return;
    }
    case Symbol::Partition: {
        // A pie, with a slice drawn out of it.
        float const turn = 2 * static_cast<float>(Math::pi);
        Path shape;
        shape.move_to(at(7.3f, 8.7f));
        shape.arc_to(at(7.3f, 8.7f), 5.9f * unit, 0, turn * 0.75f);
        shape.close();
        shape.move_to(at(8.7f, 7.3f));
        shape.arc_to(at(8.7f, 7.3f), 5.9f * unit, turn * 0.75f, turn);
        shape.close();
        m_painter.fill_path(shape, color);
        return;
    }
    case Symbol::Contrast: {
        // A disc, half of it light.
        Path shape;
        shape.add_circle(at(8, 8), 6.6f * unit);
        shape.add_circle(at(8, 8), 5.4f * unit);
        shape.move_to(at(8, 2.6f));
        shape.arc_to(at(8, 8), 5.4f * unit, -1.5707963f, 1.5707963f);
        shape.close();
        m_painter.fill_path(shape, color, FillRule::EvenOdd);
        return;
    }
    case Symbol::Photo: {
        // A frame with hills in it, and a sun.
        Path shape;
        auto const corner = at(1.4f, 2.6f);
        shape.add_rounded_rect({ corner.x, corner.y, 13.2f * unit, 10.8f * unit }, 2 * unit);
        auto const inner = at(2.6f, 3.8f);
        shape.add_rounded_rect({ inner.x, inner.y, 10.8f * unit, 8.4f * unit }, 1 * unit);
        m_painter.fill_path(shape, color, FillRule::EvenOdd);
        Path hills;
        hills.move_to(at(3.4f, 11.4f));
        hills.line_to(at(6.6f, 7.2f));
        hills.line_to(at(8.8f, 9.6f));
        hills.line_to(at(10.2f, 8.2f));
        hills.line_to(at(12.6f, 11.4f));
        hills.close();
        hills.add_circle(at(11, 5.8f), 1.1f * unit);
        m_painter.fill_path(hills, color);
        return;
    }
    case Symbol::Keyboard: {
        // Keys in a case.
        Path shape;
        auto const corner = at(0.8f, 3.6f);
        shape.add_rounded_rect({ corner.x, corner.y, 14.4f * unit, 8.8f * unit }, 1.8f * unit);
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 5; ++column) {
                auto const key = at(2.2f + static_cast<float>(column) * 2.4f, 5 + static_cast<float>(row) * 2.2f);
                shape.add_rounded_rect({ key.x, key.y, 1.6f * unit, 1.4f * unit }, 0.3f * unit);
            }
        }
        auto const space = at(4.6f, 9.4f);
        shape.add_rounded_rect({ space.x, space.y, 6.8f * unit, 1.4f * unit }, 0.3f * unit);
        m_painter.fill_path(shape, color, FillRule::EvenOdd);
        return;
    }
    case Symbol::Bell: {
        Path shape;
        shape.move_to(at(8, 1.4f));
        shape.cubic_to(at(5.2f, 1.4f), at(4, 3.6f), at(4, 6));
        shape.line_to(at(4, 9));
        shape.line_to(at(2.4f, 11.6f));
        shape.line_to(at(13.6f, 11.6f));
        shape.line_to(at(12, 9));
        shape.line_to(at(12, 6));
        shape.cubic_to(at(12, 3.6f), at(10.8f, 1.4f), at(8, 1.4f));
        shape.close();
        shape.add_circle(at(8, 13.2f), 1.5f * unit);
        m_painter.fill_path(shape, color);
        return;
    }
    case Symbol::Person: {
        Path shape;
        shape.add_circle(at(8, 4.8f), 3.2f * unit);
        shape.move_to(at(2, 14.4f));
        shape.cubic_to(at(2, 10.8f), at(4.8f, 9.2f), at(8, 9.2f));
        shape.cubic_to(at(11.2f, 9.2f), at(14, 10.8f), at(14, 14.4f));
        shape.close();
        m_painter.fill_path(shape, color);
        return;
    }
    case Symbol::Information: {
        // An i, cut out of a disc.
        Path shape;
        shape.add_circle(at(8, 8), 6.8f * unit);
        shape.add_circle(at(8, 4.7f), 1.05f * unit);
        auto const stem = at(7, 6.8f);
        shape.add_rounded_rect({ stem.x, stem.y, 2 * unit, 5.4f * unit }, 0.4f * unit);
        m_painter.fill_path(shape, color, FillRule::EvenOdd);
        return;
    }
    case Symbol::Accessibility: {
        // Someone with their arms out, in a ring.
        Path ring;
        ring.add_circle(at(8, 8), 6.9f * unit);
        ring.add_circle(at(8, 8), 5.7f * unit);
        m_painter.fill_path(ring, color, FillRule::EvenOdd);
        Path figure;
        figure.add_circle(at(8, 4.6f), 1.15f * unit);
        figure.move_to(at(4, 6));
        figure.line_to(at(12, 6));
        figure.line_to(at(12, 7));
        figure.line_to(at(9.2f, 7.4f));
        figure.line_to(at(9.2f, 9));
        figure.line_to(at(10.4f, 12.2f));
        figure.line_to(at(9.4f, 12.6f));
        figure.line_to(at(8, 9.6f));
        figure.line_to(at(6.6f, 12.6f));
        figure.line_to(at(5.6f, 12.2f));
        figure.line_to(at(6.8f, 9));
        figure.line_to(at(6.8f, 7.4f));
        figure.line_to(at(4, 7));
        figure.close();
        m_painter.fill_path(figure, color);
        return;
    }
    case Symbol::Mount: {
        // A drive with an arrow going into it.
        Path shape;
        auto const corner = at(1.4f, 8.4f);
        shape.add_rounded_rect({ corner.x, corner.y, 13.2f * unit, 5.6f * unit }, 1.4f * unit);
        auto const stem = at(7.1f, 1.6f);
        shape.add_rounded_rect({ stem.x, stem.y, 1.8f * unit, 4 * unit }, 0.3f * unit);
        shape.move_to(at(8, 7.6f));
        shape.line_to(at(5, 4.8f));
        shape.line_to(at(11, 4.8f));
        shape.close();
        m_painter.fill_path(shape, color);
        return;
    }
    case Symbol::Lock: {
        Path body;
        auto corner = at(3.2f, 7.2f);
        body.add_rounded_rect({ corner.x, corner.y, 9.6f * unit, 7 * unit }, 1.4f * unit);
        m_painter.fill_path(body, color);
        // The shackle, over the top.
        constexpr int steps = 10;
        path.move_to(at(5, 7.2f));
        path.line_to(at(5, 5));
        for (int i = 0; i <= steps; ++i) {
            float angle = 3.1415927f + 3.1415927f * static_cast<float>(i) / static_cast<float>(steps);
            path.line_to(at(8 + 3 * static_cast<float>(Math::cosine(static_cast<double>(angle))), 5 + 3 * static_cast<float>(Math::sine(static_cast<double>(angle)))));
        }
        path.line_to(at(11, 7.2f));
        style.width = 1.4f * unit;
        break;
    }
    case Symbol::Globe: {
        // The circle, the two meridians through it and two lines of latitude.
        constexpr int steps = 24;
        for (int i = 0; i <= steps; ++i) {
            float angle = 6.2831855f * static_cast<float>(i) / static_cast<float>(steps);
            auto point = at(8 + 6.2f * static_cast<float>(Math::cosine(static_cast<double>(angle))), 8 + 6.2f * static_cast<float>(Math::sine(static_cast<double>(angle))));
            if (i == 0)
                path.move_to(point);
            else
                path.line_to(point);
        }
        for (int i = 0; i <= steps; ++i) {
            float angle = 6.2831855f * static_cast<float>(i) / static_cast<float>(steps);
            auto point = at(8 + 2.7f * static_cast<float>(Math::cosine(static_cast<double>(angle))), 8 + 6.2f * static_cast<float>(Math::sine(static_cast<double>(angle))));
            if (i == 0)
                path.move_to(point);
            else
                path.line_to(point);
        }
        path.move_to(at(2.4f, 5.8f));
        path.line_to(at(13.6f, 5.8f));
        path.move_to(at(2.4f, 10.2f));
        path.line_to(at(13.6f, 10.2f));
        style.width = 1.1f * unit;
        break;
    }
    case Symbol::Speaker: {
        Path cone;
        cone.move_to(at(1.8f, 6));
        cone.line_to(at(4.4f, 6));
        cone.line_to(at(7.8f, 3));
        cone.line_to(at(7.8f, 13));
        cone.line_to(at(4.4f, 10));
        cone.line_to(at(1.8f, 10));
        cone.close();
        m_painter.fill_path(cone, color);
        // Two waves going out from it.
        constexpr float wave_radii[] = { 2.6f, 5.2f };
        for (float radius : wave_radii) {
            constexpr int steps = 8;
            for (int i = 0; i <= steps; ++i) {
                float angle = -0.85f + 1.7f * static_cast<float>(i) / static_cast<float>(steps);
                auto point = at(7.6f + radius * static_cast<float>(Math::cosine(static_cast<double>(angle))), 8 + radius * static_cast<float>(Math::sine(static_cast<double>(angle))));
                if (i == 0)
                    path.move_to(point);
                else
                    path.line_to(point);
            }
        }
        break;
    }
    case Symbol::Refresh: {
        // Most of a circle, and the head of the arrow on its end.
        constexpr int steps = 24;
        for (int i = 0; i <= steps; ++i) {
            // From a little past the top, the long way round, and back up to it.
            float angle = -0.75f + 5.46f * static_cast<float>(i) / static_cast<float>(steps);
            auto point = at(8 + 5.2f * static_cast<float>(Math::cosine(static_cast<double>(angle))), 8 + 5.2f * static_cast<float>(Math::sine(static_cast<double>(angle))));
            if (i == 0)
                path.move_to(point);
            else
                path.line_to(point);
        }
        path.move_to(at(6.6f, 0.7f));
        path.line_to(at(8.9f, 2.8f));
        path.line_to(at(6.6f, 4.9f));
        break;
    }
    case Symbol::Folder: {
        Path folder;
        folder.move_to(at(2, 4.4f));
        folder.line_to(at(6.2f, 4.4f));
        folder.line_to(at(7.6f, 5.8f));
        folder.line_to(at(14, 5.8f));
        folder.line_to(at(14, 12.6f));
        folder.line_to(at(2, 12.6f));
        folder.close();
        m_painter.fill_path(folder, color);
        return;
    }
    case Symbol::File: {
        Path file;
        file.move_to(at(4, 2));
        file.line_to(at(9.6f, 2));
        file.line_to(at(12.4f, 4.8f));
        file.line_to(at(12.4f, 14));
        file.line_to(at(4, 14));
        file.close();
        m_painter.fill_path(file, color);
        return;
    }
    case Symbol::Sidebar:
        path.add_rounded_rect({ at(2, 3.4f).x, at(2, 3.4f).y, 12 * unit, 9.2f * unit }, 1.8f * unit);
        path.move_to(at(6.4f, 3.4f));
        path.line_to(at(6.4f, 12.6f));
        break;
    }
    m_painter.stroke_path(path, style, color);
}

}
