/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Console/Framebuffer.h>
#include <LibBase/StdLibExtras.h>

namespace Kernel {

void Framebuffer::initialize(API::FramebufferInfo const& info)
{
    if (info.format == API::PixelFormat::None || info.width == 0 || info.height == 0 || info.pitch < info.width * 4)
        return;
    m_base = reinterpret_cast<u8*>(API::direct_map_base + info.physical_base);
    m_width = info.width;
    m_height = info.height;
    m_pitch = info.pitch;
    m_format = info.format;
}

u32 Framebuffer::pack(Color color) const
{
    // Pixels are little-endian 32-bit words; the format names the byte order.
    if (m_format == API::PixelFormat::BGRX8888)
        return (u32(color.red) << 16) | (u32(color.green) << 8) | color.blue;
    return (u32(color.blue) << 16) | (u32(color.green) << 8) | color.red;
}

Color Framebuffer::unpack(u32 pixel) const
{
    auto high = static_cast<u8>(pixel >> 16);
    auto middle = static_cast<u8>(pixel >> 8);
    auto low = static_cast<u8>(pixel);
    if (m_format == API::PixelFormat::BGRX8888)
        return { high, middle, low };
    return { low, middle, high };
}

void Framebuffer::fill_rect(i32 x, i32 y, i32 width, i32 height, Color color)
{
    i32 left = max(x, 0);
    i32 top = max(y, 0);
    i32 right = min(x + width, static_cast<i32>(m_width));
    i32 bottom = min(y + height, static_cast<i32>(m_height));
    u32 pixel = pack(color);
    for (i32 row_index = top; row_index < bottom; ++row_index) {
        u32* pixels = row(static_cast<u32>(row_index));
        for (i32 column = left; column < right; ++column)
            pixels[column] = pixel;
    }
}

void Framebuffer::blend_pixel(i32 x, i32 y, Color color, u32 coverage)
{
    if (x < 0 || y < 0 || x >= static_cast<i32>(m_width) || y >= static_cast<i32>(m_height) || coverage == 0)
        return;
    u32& pixel = row(static_cast<u32>(y))[x];
    if (coverage >= 256) {
        pixel = pack(color);
        return;
    }
    Color under = unpack(pixel);
    auto mix = [coverage](u8 over, u8 below) {
        return static_cast<u8>((u32(over) * coverage + u32(below) * (256 - coverage)) >> 8);
    };
    pixel = pack({ mix(color.red, under.red), mix(color.green, under.green), mix(color.blue, under.blue) });
}

void Framebuffer::scroll_up(u32 rows, Color fill)
{
    rows = min(rows, m_height);
    for (u32 y = 0; y + rows < m_height; ++y)
        __builtin_memcpy(row(y), row(y + rows), u64(m_width) * 4);
    fill_rect(0, static_cast<i32>(m_height - rows), static_cast<i32>(m_width), static_cast<i32>(rows), fill);
}

Framebuffer& boot_framebuffer()
{
    static Framebuffer framebuffer;
    return framebuffer;
}

}
