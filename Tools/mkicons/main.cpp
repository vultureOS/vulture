/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Painter.h>
#include <LibImage/Image.h>

// mkicons DIRECTORY: every icon of the system, drawn here in code and written as
// .vicns files: the same drawing at each size. All of it is original art.

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fwrite(void const* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
int printf(char const* format, ...);
}

using namespace Gfx;
namespace Math = Base::Math;

namespace {

// Everything is drawn in a square of 0 to 1.
class Canvas {
public:
    Canvas(Painter& painter, float size)
        : m_painter(painter)
        , m_size(size)
    {
    }

    [[nodiscard]] FloatPoint at(float x, float y) const { return { x * m_size, y * m_size }; }
    [[nodiscard]] FloatRect rect(float x, float y, float width, float height) const { return { x * m_size, y * m_size, width * m_size, height * m_size }; }
    [[nodiscard]] float size() const { return m_size; }
    [[nodiscard]] Painter& painter() { return m_painter; }

    void fill_rect(float x, float y, float width, float height, Color color) { m_painter.fill_rect(rect(x, y, width, height), color); }

    void rounded(float x, float y, float width, float height, float radius, Color color)
    {
        m_painter.fill_rounded_rect(rect(x, y, width, height), radius * m_size, color);
    }

    void rounded_gradient(float x, float y, float width, float height, float radius, Color top, Color bottom)
    {
        Path path;
        path.add_rounded_rect(rect(x, y, width, height), radius * m_size);
        GradientStop stops[2] = { { 0, top }, { 1, bottom } };
        m_painter.fill_path(path, Gradient::linear(at(x, y), at(x, y + height), Span<GradientStop const>(stops, 2)));
    }

    void circle(float x, float y, float radius, Color color)
    {
        Path path;
        path.add_circle(at(x, y), radius * m_size);
        m_painter.fill_path(path, color);
    }

    void line(float x0, float y0, float x1, float y1, float width, Color color, bool round = true)
    {
        Path path;
        path.move_to(at(x0, y0));
        path.line_to(at(x1, y1));
        StrokeStyle style;
        style.width = width * m_size;
        if (round)
            style.cap = LineCap::Round;
        m_painter.stroke_path(path, style, color);
    }

    void polygon(Span<FloatPoint const> points, Color color)
    {
        Path path;
        path.move_to(at(points[0].x, points[0].y));
        for (size_t i = 1; i < points.size(); ++i)
            path.line_to(at(points[i].x, points[i].y));
        path.close();
        m_painter.fill_path(path, color);
    }

    // The shape of an application's icon: a soft shadow, and a rounded square with
    // continuous corners, lit from above.
    void app_base(Color top, Color bottom)
    {
        constexpr float margin = 0.098f;
        constexpr float side = 1 - margin * 2;
        constexpr float radius = 0.185f;
        for (int i = 0; i < 8; ++i) {
            float grown = static_cast<float>(8 - i) * 0.004f;
            m_painter.fill_continuous_rounded_rect(rect(margin - grown, margin - grown + 0.012f, side + grown * 2, side + grown * 2), (radius + grown) * m_size, Color { 0, 0, 0, 10 });
        }
        Path path;
        path.add_continuous_rounded_rect(rect(margin, margin, side, side), radius * m_size);
        GradientStop stops[2] = { { 0, top }, { 1, bottom } };
        m_painter.fill_path(path, Gradient::linear(at(margin, margin), at(margin, margin + side), Span<GradientStop const>(stops, 2)));
        // A thin edge, light above.
        Path edge;
        edge.add_continuous_rounded_rect(rect(margin, margin, side, side), radius * m_size);
        edge.add_continuous_rounded_rect(rect(margin + 0.004f, margin + 0.004f, side - 0.008f, side - 0.008f), (radius - 0.004f) * m_size);
        m_painter.fill_path(edge, Color { 255, 255, 255, 40 }, FillRule::EvenOdd);
    }

private:
    Painter& m_painter;
    float m_size;
};

Color rgb(u32 value, u8 alpha = 255)
{
    return Color::from_rgb(value, alpha);
}

void folder_shape(Canvas& c, float x, float y, float width, float height, Color back, Color front_top, Color front_bottom)
{
    float tab = height * 0.16f;
    c.rounded(x, y, width * 0.46f, height * 0.4f, height * 0.07f, back);
    c.rounded(x, y + tab * 0.6f, width, height - tab * 0.6f, height * 0.07f, back);
    c.rounded_gradient(x, y + tab * 1.2f, width, height - tab * 1.2f, height * 0.07f, front_top, front_bottom);
}

void draw_gear(Canvas& c, float cx, float cy, float outer, float inner, int teeth, Color color, Color hole)
{
    Path path;
    constexpr float two_pi = 2 * static_cast<float>(Math::pi);
    float step = two_pi / static_cast<float>(teeth);
    for (int i = 0; i < teeth; ++i) {
        float a = static_cast<float>(i) * step;
        float parts[4] = { -0.30f, -0.17f, 0.17f, 0.30f };
        float radii[4] = { inner, outer, outer, inner };
        for (int j = 0; j < 4; ++j) {
            float angle = a + parts[j] * step;
            auto point = c.at(cx + radii[j] * static_cast<float>(Math::cosine(static_cast<double>(angle))), cy + radii[j] * static_cast<float>(Math::sine(static_cast<double>(angle))));
            if (i == 0 && j == 0)
                path.move_to(point);
            else
                path.line_to(point);
        }
    }
    path.close();
    c.painter().fill_path(path, color);
    c.circle(cx, cy, inner * 0.52f, hole);
}

void draw_magnifier(Canvas& c, float cx, float cy, float radius, float weight, Color color)
{
    Path ring;
    ring.add_circle(c.at(cx, cy), radius * c.size());
    StrokeStyle style;
    style.width = weight * c.size();
    c.painter().stroke_path(ring, style, color);
    float d = radius * 0.72f;
    c.line(cx + d, cy + d, cx + d + radius * 0.82f, cy + d + radius * 0.82f, weight * 1.25f, color);
}

using DrawIcon = void (*)(Canvas&);

void draw_files(Canvas& c)
{
    c.app_base(rgb(0x5ac8fa), rgb(0x0a60d0));
    folder_shape(c, 0.22f, 0.27f, 0.56f, 0.44f, rgb(0xffffff, 170), rgb(0xffffff), rgb(0xe6f0fb));
    draw_magnifier(c, 0.56f, 0.56f, 0.105f, 0.03f, rgb(0x0a60d0));
    c.circle(0.56f, 0.56f, 0.09f, rgb(0xcfe8ff, 200));
}

void draw_settings(Canvas& c)
{
    c.app_base(rgb(0xd8d8dc), rgb(0x8e8e93));
    draw_gear(c, 0.5f, 0.5f, 0.27f, 0.205f, 12, rgb(0x4a4a4e), rgb(0xc9c9cd));
    draw_gear(c, 0.5f, 0.5f, 0.13f, 0.095f, 8, rgb(0x6d6d72), rgb(0xe5e5ea));
}

void draw_launcher(Canvas& c)
{
    c.app_base(rgb(0x4c4c52), rgb(0x1c1c1e));
    Color const colors[9] = { rgb(0xff3b30), rgb(0xff9500), rgb(0xffcc00), rgb(0x34c759), rgb(0x00c7be), rgb(0x0a84ff), rgb(0x5856d6), rgb(0xaf52de), rgb(0xff2d55) };
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            float px = 0.24f + static_cast<float>(x) * 0.19f;
            float py = 0.24f + static_cast<float>(y) * 0.19f;
            c.rounded(px, py, 0.14f, 0.14f, 0.035f, colors[x + y * 3]);
        }
    }
}

void draw_terminal(Canvas& c)
{
    c.app_base(rgb(0x3a3a3c), rgb(0x161618));
    c.rounded(0.15f, 0.17f, 0.70f, 0.66f, 0.05f, rgb(0x0b0b0c));
    c.fill_rect(0.15f, 0.17f, 0.70f, 0.075f, rgb(0x48484a));
    c.line(0.25f, 0.38f, 0.37f, 0.48f, 0.04f, rgb(0xf2f2f7));
    c.line(0.37f, 0.48f, 0.25f, 0.58f, 0.04f, rgb(0xf2f2f7));
    c.line(0.42f, 0.60f, 0.60f, 0.60f, 0.04f, rgb(0xf2f2f7));
}

void draw_text_editor(Canvas& c)
{
    c.app_base(rgb(0xf2f2f7), rgb(0xd1d1d6));
    c.rounded(0.26f, 0.16f, 0.48f, 0.68f, 0.02f, rgb(0xffffff));
    for (int i = 0; i < 7; ++i) {
        float y = 0.26f + static_cast<float>(i) * 0.075f;
        float w = i == 6 ? 0.18f : (i % 3 == 2 ? 0.28f : 0.36f);
        c.line(0.32f, y, 0.32f + w, y, 0.016f, rgb(0x8e8e93));
    }
    c.line(0.48f, 0.72f, 0.76f, 0.36f, 0.045f, rgb(0x0a84ff));
    FloatPoint tip[3] = { { 0.455f, 0.755f }, { 0.51f, 0.74f }, { 0.465f, 0.70f } };
    c.polygon(Span<FloatPoint const>(tip, 3), rgb(0x3a3a3c));
}

void draw_preview(Canvas& c)
{
    c.app_base(rgb(0xe5e5ea), rgb(0xaeaeb2));
    c.rounded(0.18f, 0.22f, 0.64f, 0.56f, 0.025f, rgb(0xffffff));
    c.rounded_gradient(0.21f, 0.25f, 0.58f, 0.50f, 0.015f, rgb(0x64d2ff), rgb(0xbfe9ff));
    c.circle(0.66f, 0.36f, 0.05f, rgb(0xffd60a));
    FloatPoint hill[4] = { { 0.21f, 0.75f }, { 0.40f, 0.46f }, { 0.58f, 0.75f }, { 0.21f, 0.75f } };
    c.polygon(Span<FloatPoint const>(hill, 4), rgb(0x30a14e));
    FloatPoint hill2[3] = { { 0.46f, 0.75f }, { 0.64f, 0.54f }, { 0.79f, 0.75f } };
    c.polygon(Span<FloatPoint const>(hill2, 3), rgb(0x248a3d));
}

void draw_calculator(Canvas& c)
{
    c.app_base(rgb(0x48484a), rgb(0x1c1c1e));
    c.rounded(0.22f, 0.19f, 0.56f, 0.15f, 0.025f, rgb(0xd1f0d6));
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            Color key = x == 3 ? rgb(0xff9f0a) : rgb(0x8e8e93);
            c.rounded(0.22f + static_cast<float>(x) * 0.145f, 0.40f + static_cast<float>(y) * 0.135f, 0.125f, 0.115f, 0.025f, key);
        }
    }
}

void draw_activity(Canvas& c)
{
    c.app_base(rgb(0x2c2c2e), rgb(0x000000));
    for (int i = 1; i < 4; ++i)
        c.fill_rect(0.16f, 0.16f + static_cast<float>(i) * 0.17f, 0.68f, 0.006f, rgb(0x30d158, 60));
    Path monitor;
    FloatPoint const points[] = { { 0.16f, 0.56f }, { 0.32f, 0.56f }, { 0.38f, 0.40f }, { 0.46f, 0.72f }, { 0.54f, 0.28f }, { 0.60f, 0.60f }, { 0.66f, 0.50f }, { 0.84f, 0.50f } };
    monitor.move_to(c.at(points[0].x, points[0].y));
    for (size_t i = 1; i < array_size(points); ++i)
        monitor.line_to(c.at(points[i].x, points[i].y));
    StrokeStyle style;
    style.width = 0.028f * c.size();
    style.join = LineJoin::Round;
    style.cap = LineCap::Round;
    c.painter().stroke_path(monitor, style, rgb(0x30d158));
}

void draw_console(Canvas& c)
{
    c.app_base(rgb(0x636366), rgb(0x2c2c2e));
    c.rounded(0.17f, 0.17f, 0.66f, 0.66f, 0.03f, rgb(0xf2f2f7));
    Color const marks[6] = { rgb(0xff453a), rgb(0xffd60a), rgb(0x8e8e93), rgb(0x8e8e93), rgb(0xffd60a), rgb(0x8e8e93) };
    for (int i = 0; i < 6; ++i) {
        float y = 0.25f + static_cast<float>(i) * 0.095f;
        c.circle(0.25f, y, 0.016f, marks[i]);
        c.line(0.30f, y, 0.74f - static_cast<float>(i % 3) * 0.08f, y, 0.016f, rgb(0x48484a));
    }
}

void draw_disk(Canvas& c)
{
    c.app_base(rgb(0xe5e5ea), rgb(0x98989d));
    c.circle(0.5f, 0.5f, 0.28f, rgb(0xd1d1d6));
    c.circle(0.5f, 0.5f, 0.27f, rgb(0xf2f2f7));
    c.circle(0.5f, 0.5f, 0.17f, rgb(0xc7c7cc));
    c.circle(0.5f, 0.5f, 0.06f, rgb(0x636366));
    c.line(0.5f, 0.5f, 0.70f, 0.36f, 0.024f, rgb(0x0a84ff));
    c.circle(0.70f, 0.36f, 0.026f, rgb(0x0a84ff));
}

void draw_archive(Canvas& c)
{
    c.app_base(rgb(0xd7b98e), rgb(0x9c7a4d));
    c.rounded(0.20f, 0.30f, 0.60f, 0.48f, 0.03f, rgb(0xeed9b6));
    c.rounded(0.17f, 0.22f, 0.66f, 0.16f, 0.03f, rgb(0xc9a877));
    c.fill_rect(0.46f, 0.22f, 0.08f, 0.56f, rgb(0x8a6a3f));
    for (int i = 0; i < 6; ++i) {
        float y = 0.26f + static_cast<float>(i) * 0.085f;
        c.fill_rect(0.48f, y, 0.04f, 0.036f, rgb(0xf2f2f7));
    }
}

void draw_software(Canvas& c)
{
    c.app_base(rgb(0x40c8ff), rgb(0x0040dd));
    c.circle(0.5f, 0.46f, 0.23f, rgb(0xffffff, 40));
    c.line(0.5f, 0.27f, 0.5f, 0.56f, 0.05f, rgb(0xffffff));
    c.line(0.39f, 0.46f, 0.5f, 0.57f, 0.05f, rgb(0xffffff));
    c.line(0.61f, 0.46f, 0.5f, 0.57f, 0.05f, rgb(0xffffff));
    c.rounded(0.29f, 0.68f, 0.42f, 0.05f, 0.025f, rgb(0xffffff));
}

void draw_gallery(Canvas& c)
{
    c.app_base(rgb(0xbf5af2), rgb(0xff375f));
    c.rounded(0.25f, 0.26f, 0.5f, 0.13f, 0.035f, rgb(0xffffff));
    c.rounded(0.25f, 0.45f, 0.21f, 0.11f, 0.055f, rgb(0x30d158));
    c.circle(0.405f, 0.505f, 0.044f, rgb(0xffffff));
    c.rounded(0.54f, 0.45f, 0.11f, 0.11f, 0.025f, rgb(0x0a84ff));
    c.line(0.25f, 0.68f, 0.75f, 0.68f, 0.022f, rgb(0xffffff, 180));
    c.circle(0.45f, 0.68f, 0.045f, rgb(0xffffff));
}

void draw_search(Canvas& c)
{
    c.app_base(rgb(0xc7c7cc), rgb(0x8e8e93));
    draw_magnifier(c, 0.45f, 0.45f, 0.17f, 0.05f, rgb(0xffffff));
}

void draw_overview(Canvas& c)
{
    c.app_base(rgb(0x7d7aff), rgb(0x3634a3));
    c.rounded(0.20f, 0.24f, 0.28f, 0.2f, 0.02f, rgb(0xffffff, 230));
    c.rounded(0.53f, 0.24f, 0.27f, 0.2f, 0.02f, rgb(0xffffff, 170));
    c.rounded(0.20f, 0.50f, 0.28f, 0.26f, 0.02f, rgb(0xffffff, 170));
    c.rounded(0.53f, 0.50f, 0.27f, 0.26f, 0.02f, rgb(0xffffff, 230));
}

void draw_folder(Canvas& c)
{
    folder_shape(c, 0.07f, 0.17f, 0.86f, 0.68f, rgb(0x56a8e8), rgb(0x8ad1fb), rgb(0x54aef0));
}

void draw_document(Canvas& c)
{
    FloatPoint page[5] = { { 0.20f, 0.07f }, { 0.60f, 0.07f }, { 0.80f, 0.27f }, { 0.80f, 0.93f }, { 0.20f, 0.93f } };
    for (int i = 0; i < 4; ++i)
        c.rounded(0.20f - 0.004f * static_cast<float>(i), 0.075f + 0.004f * static_cast<float>(i), 0.60f + 0.008f * static_cast<float>(i), 0.86f, 0.01f, rgb(0x000000, 10));
    c.polygon(Span<FloatPoint const>(page, 5), rgb(0xffffff));
    FloatPoint fold[3] = { { 0.60f, 0.07f }, { 0.80f, 0.27f }, { 0.60f, 0.27f } };
    c.polygon(Span<FloatPoint const>(fold, 3), rgb(0xd1d1d6));
    for (int i = 0; i < 5; ++i)
        c.line(0.30f, 0.42f + static_cast<float>(i) * 0.09f, 0.70f - static_cast<float>(i % 2) * 0.08f, 0.42f + static_cast<float>(i) * 0.09f, 0.016f, rgb(0xc7c7cc));
}

void draw_trash_body(Canvas& c)
{
    FloatPoint body[4] = { { 0.20f, 0.26f }, { 0.80f, 0.26f }, { 0.72f, 0.92f }, { 0.28f, 0.92f } };
    c.polygon(Span<FloatPoint const>(body, 4), rgb(0xd8d8dc, 235));
    for (int i = 0; i < 5; ++i) {
        float x = 0.33f + static_cast<float>(i) * 0.085f;
        c.line(x, 0.32f, 0.38f + static_cast<float>(i) * 0.06f, 0.86f, 0.012f, rgb(0x8e8e93));
    }
    c.rounded(0.16f, 0.20f, 0.68f, 0.07f, 0.03f, rgb(0xf2f2f7));
}

void draw_trash(Canvas& c)
{
    draw_trash_body(c);
}

void draw_trash_full(Canvas& c)
{
    c.rounded(0.30f, 0.06f, 0.2f, 0.2f, 0.02f, rgb(0xffffff));
    c.rounded(0.46f, 0.10f, 0.2f, 0.17f, 0.02f, rgb(0xe5e5ea));
    draw_trash_body(c);
}

void draw_user(Canvas& c)
{
    c.circle(0.5f, 0.5f, 0.46f, rgb(0xaeaeb2));
    c.circle(0.5f, 0.38f, 0.15f, rgb(0xffffff));
    Path shoulders;
    shoulders.add_ellipse(c.rect(0.22f, 0.58f, 0.56f, 0.42f));
    c.painter().save();
    Path disc;
    disc.add_circle(c.at(0.5f, 0.5f), 0.46f * c.size());
    c.painter().clip_path(disc);
    c.painter().fill_path(shoulders, rgb(0xffffff));
    c.painter().restore();
}

void draw_generic_app(Canvas& c)
{
    c.app_base(rgb(0xc7c7cc), rgb(0x8e8e93));
    c.line(0.32f, 0.70f, 0.70f, 0.32f, 0.05f, rgb(0xffffff));
    c.line(0.32f, 0.32f, 0.70f, 0.70f, 0.05f, rgb(0xffffff));
}

struct IconDefinition {
    char const* name;
    DrawIcon draw;
};

IconDefinition const s_icons[] = {
    { "Files", draw_files },
    { "Settings", draw_settings },
    { "Launcher", draw_launcher },
    { "Terminal", draw_terminal },
    { "TextEditor", draw_text_editor },
    { "Viewer", draw_preview },
    { "Calculator", draw_calculator },
    { "Monitor", draw_activity },
    { "Console", draw_console },
    { "Disks", draw_disk },
    { "Archiver", draw_archive },
    { "Software", draw_software },
    { "WidgetGallery", draw_gallery },
    { "Search", draw_search },
    { "Overview", draw_overview },
    { "Folder", draw_folder },
    { "Document", draw_document },
    { "Trash", draw_trash },
    { "TrashFull", draw_trash_full },
    { "User", draw_user },
    { "GenericApplication", draw_generic_app },
};

void write_file(StringView path, ReadonlyBytes bytes)
{
    auto terminated = MUST(String::from_view(path));
    FILE* file = fopen(terminated.characters(), "wb");
    VERIFY(file);
    fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
}

}

// A picture for Preview to open from the start: dunes at dusk, in nobody's style but ours.
bool write_sample_picture(StringView directory)
{
    constexpr int width = 1600, height = 1000;
    auto bitmap = MUST(Bitmap::create({ width, height }));
    Painter painter(*bitmap);
    GradientStop const stops[] = {
        { 0, Color { 34, 31, 78, 255 } },
        { 0.55f, Color { 197, 88, 112, 255 } },
        { 1, Color { 252, 190, 120, 255 } },
    };
    Path whole;
    whole.add_rect({ 0, 0, width, height });
    painter.fill_path(whole, Gradient::linear({ 0, 0 }, { 0, static_cast<float>(height) }, { stops, array_size(stops) }));
    Canvas canvas(painter, static_cast<float>(width));
    canvas.circle(0.68f, 0.30f, 0.055f, Color { 255, 236, 200, 255 });
    struct Dune {
        float base;
        float rise;
        float phase;
        Color color;
    };
    Dune const dunes[] = {
        { 0.70f, 0.055f, 0.4f, Color { 150, 70, 104, 255 } },
        { 0.80f, 0.060f, 1.9f, Color { 104, 50, 92, 255 } },
        { 0.90f, 0.050f, 3.2f, Color { 66, 36, 78, 255 } },
    };
    for (auto const& dune : dunes) {
        Path path;
        path.move_to(FloatPoint { 0, static_cast<float>(height) });
        constexpr int steps = 96;
        for (int i = 0; i <= steps; ++i) {
            float x = static_cast<float>(i) / static_cast<float>(steps);
            float wave = static_cast<float>(Math::sine(static_cast<double>(x * 7.0f + dune.phase))) + 0.35f * static_cast<float>(Math::sine(static_cast<double>(x * 17.0f + dune.phase * 2)));
            path.line_to(FloatPoint { x * static_cast<float>(width), (dune.base + dune.rise * wave) * static_cast<float>(height) });
        }
        path.line_to(FloatPoint { static_cast<float>(width), static_cast<float>(height) });
        path.close();
        painter.fill_path(path, dune.color);
    }
    auto png = MUST(Image::encode_png(*bitmap));
    StringBuilder name;
    name.append(directory);
    name.append("/Dunes.png"_sv);
    auto path = MUST(name.to_string());
    FILE* file = fopen(path.characters(), "wb");
    if (!file || fwrite(png.data(), 1, png.size(), file) != png.size()) {
        printf("mkicons: cannot write %s\n", path.characters());
        return false;
    }
    fclose(file);
    return true;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        printf("usage: mkicons DIRECTORY\n");
        return 2;
    }
    StringView directory { argv[1], __builtin_strlen(argv[1]) };
    struct Size {
        u32 points;
        u32 scale;
    };
    Size const sizes[] = { { 16, 1 }, { 16, 2 }, { 32, 1 }, { 32, 2 }, { 64, 2 }, { 128, 2 }, { 256, 2 } };
    for (auto const& icon : s_icons) {
        Vector<Vector<u8>> pngs;
        for (auto const& size : sizes) {
            int pixels = static_cast<int>(size.points * size.scale);
            auto bitmap = MUST(Bitmap::create({ pixels, pixels }));
            Painter painter(*bitmap);
            Canvas canvas(painter, static_cast<float>(pixels));
            icon.draw(canvas);
            pngs.append(MUST(Image::encode_png(*bitmap)));
        }
        Vector<Image::IconFile::Entry> entries;
        for (size_t i = 0; i < array_size(sizes); ++i)
            entries.append(Image::IconFile::Entry { sizes[i].points, sizes[i].scale, pngs[i].span() });
        auto file = MUST(Image::encode_icon_file(entries));
        StringBuilder path;
        path.append(directory);
        path.append("/"_sv);
        path.append(StringView(icon.name, __builtin_strlen(icon.name)));
        path.append(".vicns"_sv);
        write_file(path.view(), file.span());
    }
    return write_sample_picture(directory) ? 0 : 1;
}
