/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Cursors.h"
#include <LibGfx/Filter.h>
#include <LibGfx/Painter.h>

// The pointer's shapes, in the manner of the Mac's before Big Sur: black with a white edge, and a soft shadow. This is
// original art, not a copy of anyone's pictures, and it's drawn rather than stored, in points, for the screen's scale.
//
// Each shape is laid out so that it's sharp at 1x, where a blurred edge shows: what should be a straight edge is on a
// pixel boundary, a slanted one is at exactly 45 degrees or 2 to 1, and a line one point wide is centered in a pixel.

namespace WindowServer {

using namespace Gfx;
using Window::CursorShape;

namespace {

constexpr float pi = 3.14159265f;
// How far a shape's white edge goes beyond its black, in points. A stroke is half outside what it strokes.
constexpr float outline_width = 2;
constexpr float shadow_opacity = 0.42f;
constexpr float shadow_sigma = 1.1f;
constexpr int shadow_drop = 1;

Color const ink = Color::from_rgb(0x000000);
Color const edge = Color::from_rgb(0xffffff);

void draw_outlined(Painter& painter, Path const& shape)
{
    StrokeStyle stroke;
    stroke.width = outline_width;
    stroke.join = LineJoin::Round;
    stroke.cap = LineCap::Round;
    painter.stroke_path(shape, stroke, edge);
    painter.fill_path(shape, ink);
}

void draw_thin_line(Painter& painter, Path const& line)
{
    StrokeStyle under;
    under.width = 3;
    under.join = LineJoin::Round;
    under.cap = LineCap::Butt;
    painter.stroke_path(line, under, edge);
    StrokeStyle over;
    over.width = 1;
    over.join = LineJoin::Miter;
    over.cap = LineCap::Butt;
    painter.stroke_path(line, over, ink);
}

// The arrow, with its tip at the origin: down the left side, in to the tail, round it, and back up the diagonal.
void draw_arrow(Painter& painter)
{
    Path arrow;
    arrow.move_to({ 0, 0 });
    arrow.line_to({ 0, 15 });
    arrow.line_to({ 3, 12 });
    arrow.line_to({ 5.5f, 17 });
    arrow.line_to({ 7.5f, 16 });
    arrow.line_to({ 5, 11 });
    arrow.line_to({ 11, 11 });
    arrow.close();
    draw_outlined(painter, arrow);
}

// Along x, around the origin: painters turn it where it is wanted.
void draw_double_arrow(Painter& painter)
{
    Path arrow;
    arrow.move_to({ -9, 0 });
    arrow.line_to({ -5, -4 });
    arrow.line_to({ -5, -1 });
    arrow.line_to({ 5, -1 });
    arrow.line_to({ 5, -4 });
    arrow.line_to({ 9, 0 });
    arrow.line_to({ 5, 4 });
    arrow.line_to({ 5, 1 });
    arrow.line_to({ -5, 1 });
    arrow.line_to({ -5, 4 });
    arrow.close();
    draw_outlined(painter, arrow);
}

// A white hand with a black edge: all its parts stroked first, then all filled, so that the edge is only round the
// whole, and between the fingers, where a point's gap is left.
struct HandPart {
    FloatRect rect;
    float radius;
};

void draw_hand_parts(Painter& painter, Span<HandPart const> parts, Path const* thumb)
{
    StrokeStyle stroke;
    stroke.width = outline_width;
    stroke.join = LineJoin::Round;
    for (auto const& part : parts) {
        Path path;
        path.add_rounded_rect(part.rect, part.radius);
        painter.stroke_path(path, stroke, ink);
    }
    StrokeStyle thumb_style;
    thumb_style.cap = LineCap::Round;
    if (thumb) {
        thumb_style.width = 3 + outline_width;
        painter.stroke_path(*thumb, thumb_style, ink);
    }
    for (auto const& part : parts) {
        Path path;
        path.add_rounded_rect(part.rect, part.radius);
        painter.fill_path(path, edge);
    }
    if (thumb) {
        thumb_style.width = 3;
        painter.stroke_path(*thumb, thumb_style, edge);
    }
}

// Its fingertip at the origin, as that's where it points.
void draw_pointing_hand(Painter& painter)
{
    HandPart const parts[] = {
        { { -1, 0, 3, 13 }, 1.5f },
        { { 3, 6, 3, 8 }, 1.5f },
        { { 7, 7, 3, 7 }, 1.5f },
        { { 11, 8, 2.5f, 6 }, 1.25f },
        { { -1, 11, 14.5f, 8 }, 3.5f },
    };
    Path thumb;
    thumb.move_to({ -4.5f, 9.5f });
    thumb.line_to({ -1, 14 });
    draw_hand_parts(painter, { parts, array_size(parts) }, &thumb);
}

// Around its palm's middle, as that's what it holds things by. Closed, its fingers are knuckles over the palm.
void draw_flat_hand(Painter& painter, bool closed)
{
    float const top = closed ? -5 : -8;
    float const palm_top = closed ? -1 : -2;
    HandPart const parts[] = {
        { { -7, top + 2, 3, 8 }, 1.5f },
        { { -3, top, 3, 8 }, 1.5f },
        { { 1, top, 3, 8 }, 1.5f },
        { { 5, top + 1.5f, 2.5f, 8 }, 1.25f },
        { { -7, palm_top, 14.5f, 9 - palm_top }, 3.5f },
    };
    if (closed) {
        draw_hand_parts(painter, { parts, array_size(parts) }, nullptr);
        return;
    }
    Path thumb;
    thumb.move_to({ -10.5f, -1 });
    thumb.line_to({ -7, 3.5f });
    draw_hand_parts(painter, { parts, array_size(parts) }, &thumb);
}

// Under a shape: the shape again, black, blurred, and a little lower.
NonnullRefPtr<Bitmap> with_shadow(Bitmap const& shape, int scale)
{
    auto shadow = MUST(Bitmap::create(shape.size()));
    for (int y = 0; y < shape.height(); ++y) {
        for (int x = 0; x < shape.width(); ++x) {
            auto const alpha = static_cast<float>(shape.pixel_at(x, y) >> 24);
            shadow->set_pixel_at(x, y, static_cast<Pixel>(alpha * shadow_opacity + 0.5f) << 24);
        }
    }
    MUST(gaussian_blur(*shadow, { 0, 0, shadow->width(), shadow->height() }, shadow_sigma * static_cast<float>(scale)));
    auto result = MUST(Bitmap::create(shape.size()));
    Painter painter(*result);
    painter.blit(*shadow, { 0, shadow_drop * scale }, { 0, 0, shadow->width(), shadow->height() - shadow_drop * scale });
    painter.blit(shape, { 0, 0 }, { 0, 0, shape.width(), shape.height() });
    return result;
}

}

CursorImage make_cursor(CursorShape shape, int scale)
{
    auto layer = MUST(Bitmap::create({ VULTURE_CURSOR_IMAGE_SIZE, VULTURE_CURSOR_IMAGE_SIZE }));
    Painter painter(*layer);
    auto const s = static_cast<float>(scale);
    painter.scale(s, s);
    // Where the hot spot goes in the picture, in whole points, leaving room for the edge and shadow.
    IntPoint hot { 16, 16 };
    switch (shape) {
    case CursorShape::Arrow:
    case CursorShape::Count:
        hot = { 6, 6 };
        painter.translate(6, 6);
        draw_arrow(painter);
        break;
    case CursorShape::IBeam: {
        painter.translate(16, 16);
        Path beam;
        beam.move_to({ 0.5f, -6.5f });
        beam.line_to({ 0.5f, 7.5f });
        beam.move_to({ -3, -7.5f });
        beam.line_to({ -0.5f, -7.5f });
        beam.line_to({ 0.5f, -6.5f });
        beam.line_to({ 1.5f, -7.5f });
        beam.line_to({ 4, -7.5f });
        beam.move_to({ -3, 8.5f });
        beam.line_to({ -0.5f, 8.5f });
        beam.line_to({ 0.5f, 7.5f });
        beam.line_to({ 1.5f, 8.5f });
        beam.line_to({ 4, 8.5f });
        draw_thin_line(painter, beam);
        break;
    }
    case CursorShape::PointingHand:
        hot = { 12, 6 };
        painter.translate(12, 6);
        draw_pointing_hand(painter);
        break;
    case CursorShape::ResizeLeftRight:
        painter.translate(16, 16);
        draw_double_arrow(painter);
        break;
    case CursorShape::ResizeUpDown:
        painter.translate(16, 16);
        painter.rotate(pi / 2);
        draw_double_arrow(painter);
        break;
    case CursorShape::ResizeDiagonalDown:
        painter.translate(16, 16);
        painter.rotate(pi / 4);
        draw_double_arrow(painter);
        break;
    case CursorShape::ResizeDiagonalUp:
        painter.translate(16, 16);
        painter.rotate(-pi / 4);
        draw_double_arrow(painter);
        break;
    case CursorShape::OpenHand:
        // (Its thumb goes out to the left, further than it does to the right.)
        hot = { 18, 15 };
        painter.translate(18, 15);
        draw_flat_hand(painter, false);
        break;
    case CursorShape::ClosedHand:
        painter.translate(16, 16);
        draw_flat_hand(painter, true);
        break;
    case CursorShape::Crosshair: {
        painter.translate(16, 16);
        Path cross;
        cross.move_to({ -7, 0.5f });
        cross.line_to({ 8, 0.5f });
        cross.move_to({ 0.5f, -7 });
        cross.line_to({ 0.5f, 8 });
        draw_thin_line(painter, cross);
        break;
    }
    }
    return { with_shadow(*layer, scale), { hot.x * scale, hot.y * scale } };
}

}
