/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Cursors.h"
#include <LibGfx/Painter.h>

// The pointer's shapes: original art, in points, drawn at the screen's scale.

namespace WindowServer {

using namespace Gfx;
using Window::CursorShape;

namespace {

constexpr float pi = 3.14159265f;

Color const ink = Color::from_rgb(0x000000);
Color const edge = Color::from_rgb(0xffffff);

void draw_outlined(Painter& painter, Path const& shape, float outline = 1.5f)
{
    StrokeStyle stroke;
    stroke.width = outline;
    stroke.join = LineJoin::Round;
    stroke.cap = LineCap::Round;
    painter.stroke_path(shape, stroke, edge);
    painter.fill_path(shape, ink);
}

void draw_line(Painter& painter, Path const& line, float width)
{
    StrokeStyle under;
    under.width = width + 1.5f;
    under.cap = LineCap::Round;
    painter.stroke_path(line, under, edge);
    StrokeStyle over;
    over.width = width;
    over.cap = LineCap::Round;
    painter.stroke_path(line, over, ink);
}

void draw_double_arrow(Painter& painter)
{
    // Along x, around the origin: painters turn it where it is wanted.
    Path arrow;
    arrow.move_to({ -9, 0 });
    arrow.line_to({ -4.5f, -4.5f });
    arrow.line_to({ -4.5f, -1.5f });
    arrow.line_to({ 4.5f, -1.5f });
    arrow.line_to({ 4.5f, -4.5f });
    arrow.line_to({ 9, 0 });
    arrow.line_to({ 4.5f, 4.5f });
    arrow.line_to({ 4.5f, 1.5f });
    arrow.line_to({ -4.5f, 1.5f });
    arrow.line_to({ -4.5f, 4.5f });
    arrow.close();
    draw_outlined(painter, arrow);
}

void draw_hand(Painter& painter, bool closed)
{
    Path hand;
    // The palm, then four fingers, straight or curled over it.
    hand.add_rounded_rect({ 3, 8, 12, 9 }, 4);
    float length = closed ? 2.5f : 6;
    for (int i = 0; i < 4; ++i) {
        float x = 3.5f + static_cast<float>(i) * 2.9f;
        float reach = i == 0 || i == 3 ? length - 1 : length;
        hand.add_rounded_rect({ x, 9 - reach, 2.4f, reach + 2 }, 1.2f);
    }
    if (!closed)
        hand.add_rounded_rect({ 0.5f, 9, 4, 2.4f }, 1.2f);
    StrokeStyle stroke;
    stroke.width = 1.5f;
    stroke.join = LineJoin::Round;
    painter.stroke_path(hand, stroke, ink);
    painter.fill_path(hand, edge);
}

}

CursorImage make_cursor(CursorShape shape, int scale)
{
    auto image = MUST(Bitmap::create({ 64, 64 }));
    Painter painter(*image);
    auto s = static_cast<float>(scale);
    painter.scale(s, s);
    FloatPoint hot { 0, 0 };
    switch (shape) {
    case CursorShape::Arrow:
    case CursorShape::Count: {
        hot = { 1.5f, 1 };
        painter.translate(hot.x, hot.y);
        Path arrow;
        arrow.move_to({ 0, 0 });
        arrow.line_to({ 0, 15 });
        arrow.line_to({ 3.6f, 11.6f });
        arrow.line_to({ 6.2f, 17.4f });
        arrow.line_to({ 8.8f, 16.2f });
        arrow.line_to({ 6.2f, 10.6f });
        arrow.line_to({ 11, 10.6f });
        arrow.close();
        draw_outlined(painter, arrow);
        break;
    }
    case CursorShape::IBeam: {
        hot = { 5, 9 };
        painter.translate(hot.x, hot.y);
        Path beam;
        beam.move_to({ 0, -7 });
        beam.line_to({ 0, 7 });
        beam.move_to({ -3, -8 });
        beam.line_to({ -1, -8 });
        beam.line_to({ 0, -7 });
        beam.line_to({ 1, -8 });
        beam.line_to({ 3, -8 });
        beam.move_to({ -3, 8 });
        beam.line_to({ -1, 8 });
        beam.line_to({ 0, 7 });
        beam.line_to({ 1, 8 });
        beam.line_to({ 3, 8 });
        draw_line(painter, beam, 1);
        break;
    }
    case CursorShape::PointingHand: {
        hot = { 6, 1 };
        Path hand;
        hand.add_rounded_rect({ 4.8f, 1, 2.6f, 11 }, 1.3f);
        hand.add_rounded_rect({ 7.6f, 6, 2.5f, 7 }, 1.25f);
        hand.add_rounded_rect({ 10.2f, 6.8f, 2.5f, 6 }, 1.25f);
        hand.add_rounded_rect({ 12.8f, 7.6f, 2.4f, 5 }, 1.2f);
        hand.add_rounded_rect({ 4.8f, 10, 10.4f, 7 }, 3.5f);
        // The thumb, across the palm's edge.
        Path thumb;
        thumb.move_to({ 2, 9.5f });
        thumb.line_to({ 5.5f, 13 });
        StrokeStyle stroke;
        stroke.width = 1.5f;
        stroke.join = LineJoin::Round;
        painter.stroke_path(hand, stroke, ink);
        StrokeStyle thumb_outline;
        thumb_outline.width = 4;
        thumb_outline.cap = LineCap::Round;
        painter.stroke_path(thumb, thumb_outline, ink);
        thumb_outline.width = 2.5f;
        painter.stroke_path(thumb, thumb_outline, edge);
        painter.fill_path(hand, edge);
        break;
    }
    case CursorShape::ResizeLeftRight:
        hot = { 11, 7 };
        painter.translate(hot.x, hot.y);
        draw_double_arrow(painter);
        break;
    case CursorShape::ResizeUpDown:
        hot = { 7, 11 };
        painter.translate(hot.x, hot.y);
        painter.rotate(pi / 2);
        draw_double_arrow(painter);
        break;
    case CursorShape::ResizeDiagonalDown:
        hot = { 10, 10 };
        painter.translate(hot.x, hot.y);
        painter.rotate(pi / 4);
        draw_double_arrow(painter);
        break;
    case CursorShape::ResizeDiagonalUp:
        hot = { 10, 10 };
        painter.translate(hot.x, hot.y);
        painter.rotate(-pi / 4);
        draw_double_arrow(painter);
        break;
    case CursorShape::OpenHand:
        hot = { 8, 8 };
        draw_hand(painter, false);
        break;
    case CursorShape::ClosedHand:
        hot = { 8, 8 };
        draw_hand(painter, true);
        break;
    case CursorShape::Crosshair: {
        hot = { 9, 9 };
        painter.translate(hot.x, hot.y);
        Path cross;
        cross.move_to({ -7, 0 });
        cross.line_to({ 7, 0 });
        cross.move_to({ 0, -7 });
        cross.line_to({ 0, 7 });
        draw_line(painter, cross, 1);
        break;
    }
    }
    return { move(image), { static_cast<int>(hot.x * s), static_cast<int>(hot.y * s) } };
}

}
