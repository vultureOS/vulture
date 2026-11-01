/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCore/EventLoop.h>
#include <LibGfx/Painter.h>
#include <LibText/Font.h>
#include <LibWindow/Window.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// A still scene for golden screenshots: a window with a vibrant sidebar over two
// others, the same on every run. Says "ui: ready" when it is all on screen.

namespace {

using namespace Gfx;

NonnullRefPtr<Text::Typeface> load_face(char const* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    VERIFY(fd >= 0);
    Vector<u8> bytes;
    u8 chunk[65536];
    ssize_t got;
    while ((got = read(fd, chunk, sizeof(chunk))) > 0) {
        size_t at = bytes.size();
        MUST(bytes.try_resize(at + static_cast<size_t>(got)));
        memcpy(bytes.data() + at, chunk, static_cast<size_t>(got));
    }
    close(fd);
    return MUST(Text::Typeface::parse(move(bytes)));
}

Color const ink = Color::from_rgb(0x1d1d1f);
Color const secondary = Color::from_rgb(0x6e6e73);

}

int main()
{
    Core::EventLoop loop;
    auto connection = MUST(Window::Connection::connect(loop));
    int scale_i = connection->scale();
    auto scale = static_cast<float>(scale_i);
    auto screen = connection->screen_size();

    Text::GlyphAtlas atlas;
    auto regular = load_face("/System/Library/Fonts/Inter-Regular.ttf");
    auto medium = load_face("/System/Library/Fonts/Inter-Medium.ttf");
    Text::Font body(regular, 13, scale, atlas);
    Text::Font small(regular, 11, scale, atlas);
    Text::Font large(medium, 26, scale, atlas);
    Text::Font heading(medium, 13, scale, atlas);

    int left = screen.width / 2 - 460;
    int top = screen.height / 2 - 300;

    // Behind: a colorful one, which the front window's material blurs.
    auto back = MUST(Window::Window::create(*connection, "Colors"_sv, { left - 40, top + 60, 520, 420 }, Window::WindowTitled));
    {
        auto& bitmap = back->back_buffer();
        Painter painter(bitmap);
        GradientStop const stops[] = { { 0, Color::from_rgb(0xff5e62) }, { 0.5f, Color::from_rgb(0xffc371) }, { 1, Color::from_rgb(0x47c1ff) } };
        Path all;
        all.add_rect(bitmap.rect().to<float>());
        painter.fill_path(all, Gradient::linear({ 0, 0 }, { static_cast<float>(bitmap.width()), static_cast<float>(bitmap.height()) }, { stops, 3 }));
        back->present();
    }

    // Another behind, to the right: plain text.
    auto notes = MUST(Window::Window::create(*connection, "Notes"_sv, { left + 520, top - 60, 380, 300 }, Window::WindowTitled));
    {
        auto& bitmap = notes->back_buffer();
        bitmap.clear(Color::from_rgb(0xffffff));
        Painter painter(bitmap);
        char const* lines[] = { "Everything here is drawn by our own code:", "the rasterizer, the font engine, the blur.", "", "Inter is set without hinting, at quarter-pixel", "positions, on a whole-pixel baseline." };
        float y = 56 * scale;
        for (auto const* line : lines) {
            body.draw(painter, { 16 * scale, y }, StringView(line), ink);
            y += 20 * scale;
        }
        notes->present();
    }

    // In front: a sidebar with a material behind it.
    auto front = MUST(Window::Window::create(*connection, "Documents"_sv, { left, top, 760, 500 }, Window::WindowTitled | Window::WindowResizable));
    {
        Window::Window::MaterialRegion const regions[] = { { { 0, 0, 200, 500 }, Window::Material::Sidebar } };
        front->set_materials({ regions, 1 });
        auto& bitmap = front->back_buffer();
        Painter painter(bitmap);
        // Clear in the sidebar: the material shows there.
        painter.fill_rect({ 200 * scale, 0, static_cast<float>(bitmap.width()) - 200 * scale, static_cast<float>(bitmap.height()) }, Color::from_rgb(0xffffff));
        painter.fill_rect({ 200 * scale, 28 * scale, 1, static_cast<float>(bitmap.height()) }, Color { 0, 0, 0, 30 });

        heading.draw(painter, { 16 * scale, 56 * scale }, "Favorites"_sv, secondary);
        char const* items[] = { "AirDrop", "Recents", "Applications", "Desktop", "Documents", "Downloads" };
        float y = 84 * scale;
        for (int i = 0; i < 6; ++i) {
            if (i == 4) {
                Path selected;
                selected.add_rounded_rect({ 8 * scale, y - 18 * scale, 184 * scale, 26 * scale }, 5 * scale);
                painter.fill_path(selected, Color { 0, 0, 0, 26 });
            }
            body.draw(painter, { 24 * scale, y }, StringView(items[i]), ink);
            y += 28 * scale;
        }

        large.draw(painter, { 224 * scale, 76 * scale }, "Sequoia, in our own pixels"_sv, ink);
        body.draw(painter, { 224 * scale, 108 * scale }, "AV To Ta WA, kerned. résumé, naïve, €100 — “quotes” (1/2) 0123456789"_sv, ink);
        small.draw(painter, { 224 * scale, 134 * scale }, "Small text at 11 points: sphinx of black quartz, judge my vow."_sv, secondary);

        struct Tile {
            uint32_t color;
            char const* name;
        };
        Tile const tiles[] = { { 0x0a84ff, "Blue" }, { 0x30d158, "Green" }, { 0xff9f0a, "Orange" }, { 0xff453a, "Red" }, { 0xbf5af2, "Purple" }, { 0x64d2ff, "Cyan" } };
        for (int i = 0; i < 6; ++i) {
            float x = (224 + static_cast<float>(i % 3) * 170) * scale;
            float tile_y = (170 + static_cast<float>(i / 3) * 150) * scale;
            Path tile;
            tile.add_continuous_rounded_rect({ x, tile_y, 150 * scale, 110 * scale }, 14 * scale);
            Color const base = Color::from_rgb(tiles[i].color);
            GradientStop const shade[] = { { 0, base }, { 1, Color { static_cast<u8>(base.red * 3 / 4), static_cast<u8>(base.green * 3 / 4), static_cast<u8>(base.blue * 3 / 4), 255 } } };
            painter.fill_path(tile, Gradient::linear({ x, tile_y }, { x, tile_y + 110 * scale }, { shade, 2 }));
            heading.draw(painter, { x + 14 * scale, tile_y + 92 * scale }, StringView(tiles[i].name), Color::from_rgb(0xffffff));
        }

        Path line;
        line.move_to({ 224 * scale, 480 * scale });
        line.cubic_to({ 340 * scale, 420 * scale }, { 460 * scale, 510 * scale }, { 730 * scale, 470 * scale });
        StrokeStyle style;
        style.width = 2 * scale;
        style.cap = LineCap::Round;
        painter.stroke_path(line, style, Color::from_rgb(0x007aff));
        front->present();
    }

    // In the graphical session people may click the close buttons.
    back->on_close = [&] { back->show(false); };
    notes->on_close = [&] { notes->show(false); };
    front->on_close = [&] { front->show(false); };

    back->wait_until_presented(loop);
    notes->wait_until_presented(loop);
    front->wait_until_presented(loop);
    // A frame more: anything in flight has landed.
    loop.add_timer(300, false, [] {
        printf("ui: ready\n");
        fflush(stdout);
    });
    return loop.exec();
}
