/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Plugin.h"
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Painter.h>
#include <LibImage/Image.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Pictures, as themselves.

extern "C" int vulture_quickview_claims(char const* content_type, char const*)
{
    return strcmp(content_type, "public.png") == 0 || strcmp(content_type, "public.jpeg") == 0;
}

extern "C" int vulture_quickview_render(int document, char const*, unsigned most_width, unsigned most_height, VultureQuickViewPicture* picture)
{
    constexpr size_t largest = 64 * MiB;
    Vector<u8> bytes;
    u8 buffer[65536];
    for (;;) {
        ssize_t got = read(document, buffer, sizeof(buffer));
        if (got < 0)
            return errno;
        if (got == 0)
            break;
        if (bytes.size() + static_cast<size_t>(got) > largest)
            return EFBIG;
        for (ssize_t i = 0; i < got; ++i)
            bytes.append(buffer[i]);
    }
    auto decoded = Image::decode(bytes.span());
    if (decoded.is_error())
        return EINVAL;
    auto& original = *decoded.value();
    float const across = static_cast<float>(most_width) / static_cast<float>(original.width());
    float const down = static_cast<float>(most_height) / static_cast<float>(original.height());
    float const scale = min(1.f, min(across, down));
    int const width = max(1, static_cast<int>(static_cast<float>(original.width()) * scale));
    int const height = max(1, static_cast<int>(static_cast<float>(original.height()) * scale));
    auto fitted = Gfx::Bitmap::create({ width, height });
    if (fitted.is_error())
        return ENOMEM;
    Gfx::Painter painter(*fitted.value());
    painter.draw_bitmap(original, Gfx::FloatRect { 0, 0, static_cast<float>(width), static_cast<float>(height) });
    size_t const size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    picture->pixels = static_cast<unsigned*>(malloc(size));
    if (!picture->pixels)
        return ENOMEM;
    for (int y = 0; y < height; ++y)
        memcpy(picture->pixels + static_cast<size_t>(y) * static_cast<size_t>(width), fitted.value()->scanline(y), static_cast<size_t>(width) * 4);
    picture->width = static_cast<unsigned>(width);
    picture->height = static_cast<unsigned>(height);
    return 0;
}
