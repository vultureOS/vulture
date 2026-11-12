/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Plugin.h"
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Painter.h>
#include <LibText/Font.h>
#include <LibText/Typeface.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Text, on a page, as far down as there's room for.

namespace {

constexpr size_t most_text = 64 * KiB;

bool ends_with(char const* text, char const* suffix)
{
    return strcmp(text, suffix) == 0;
}

ErrorOr<NonnullRefPtr<Text::Typeface>> load_typeface()
{
    int fd = open("/System/Library/Fonts/Inter-Regular.ttf", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    Vector<u8> bytes;
    u8 buffer[65536];
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got <= 0)
            break;
        for (ssize_t i = 0; i < got; ++i)
            bytes.append(buffer[i]);
    }
    close(fd);
    return Text::Typeface::parse(move(bytes));
}

}

extern "C" int vulture_quickview_claims(char const* content_type, char const* extension)
{
    static char const* const text_types[] = { "public.plain-text", "net.daringfireball.markdown", "com.apple.log", "public.json", "com.apple.property-list", "public.xml", "public.html", "public.css",
        "public.source-code", "public.c-source", "public.c-header", "public.c-plus-plus-source", "public.c-plus-plus-header", "public.objective-c-source", "public.swift-source", "public.shell-script",
        "public.python-script", "com.netscape.javascript-source", "public.configuration", "public.comma-separated-values-text" };
    (void)extension;
    for (auto const* type : text_types) {
        if (ends_with(content_type, type))
            return 1;
    }
    return 0;
}

extern "C" int vulture_quickview_render(int document, char const*, unsigned most_width, unsigned most_height, VultureQuickViewPicture* picture)
{
    static char text[most_text];
    size_t length = 0;
    while (length < most_text) {
        ssize_t got = read(document, text + length, most_text - length);
        if (got < 0)
            return errno;
        if (got == 0)
            break;
        length += static_cast<size_t>(got);
    }
    // Asked for a square, it's a thumbnail: a page standing up, with an edge, as a Mac draws a document's.
    bool const as_page = most_width == most_height;
    int width = static_cast<int>(most_width);
    // A page, as tall as it is wide and a third again, if that fits.
    int height = min(static_cast<int>(most_height), width * 4 / 3);
    if (as_page) {
        height = static_cast<int>(most_height);
        width = max(1, height * 3 / 4);
    }
    auto page = Gfx::Bitmap::create({ width, height });
    if (page.is_error())
        return ENOMEM;
    Gfx::Painter painter(*page.value());
    painter.fill_rect(Gfx::FloatRect { 0, 0, static_cast<float>(width), static_cast<float>(height) }, Gfx::Color { 255, 255, 255, 255 });
    if (as_page) {
        float const edge = max(1.f, static_cast<float>(height) / 160);
        Gfx::Color const grey { 0, 0, 0, 46 };
        painter.fill_rect(Gfx::FloatRect { 0, 0, static_cast<float>(width), edge }, grey);
        painter.fill_rect(Gfx::FloatRect { 0, static_cast<float>(height) - edge, static_cast<float>(width), edge }, grey);
        painter.fill_rect(Gfx::FloatRect { 0, edge, edge, static_cast<float>(height) - edge * 2 }, grey);
        painter.fill_rect(Gfx::FloatRect { static_cast<float>(width) - edge, edge, edge, static_cast<float>(height) - edge * 2 }, grey);
    }

    auto typeface = load_typeface();
    if (typeface.is_error())
        return EIO;
    static Text::GlyphAtlas atlas;
    float const points = static_cast<float>(width) / 40;
    Text::Font font(typeface.release_value(), points, 1, atlas);
    float const inset = static_cast<float>(width) / 14;
    float const line = font.line_height() * 1.1f;
    float y = inset + font.ascent();
    StringView all(text, length);
    size_t start = 0;
    while (start <= length && y < static_cast<float>(height) - inset) {
        size_t end = start;
        while (end < length && text[end] != '\n')
            ++end;
        auto rest = all.substring_view(start, end - start);
        // As much as fits on a line of the page.
        size_t fitting = font.offset_at_x(rest, static_cast<float>(width) - inset * 2);
        font.draw(painter, { inset, y }, rest.substring_view(0, fitting), Gfx::Color { 30, 30, 30, 255 });
        y += line;
        start = fitting == rest.length() ? end + 1 : start + max<size_t>(fitting, 1);
    }

    size_t const size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    picture->pixels = static_cast<unsigned*>(malloc(size));
    if (!picture->pixels)
        return ENOMEM;
    for (int row = 0; row < height; ++row)
        memcpy(picture->pixels + static_cast<size_t>(row) * static_cast<size_t>(width), page.value()->scanline(row), static_cast<size_t>(width) * 4);
    picture->width = static_cast<unsigned>(width);
    picture->height = static_cast<unsigned>(height);
    return 0;
}
