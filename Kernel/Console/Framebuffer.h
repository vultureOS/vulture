/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/BootInfo.h>

namespace Kernel {

struct Color {
    u8 red { 0 };
    u8 green { 0 };
    u8 blue { 0 };
};

// The linear framebuffer the firmware set up, reached through the direct
// map. Drawing is clipped to the screen.
class Framebuffer {
public:
    void initialize(API::FramebufferInfo const&);

    [[nodiscard]] bool is_present() const { return m_base != nullptr; }
    [[nodiscard]] u32 width() const { return m_width; }
    [[nodiscard]] u32 height() const { return m_height; }

    void fill_rect(i32 x, i32 y, i32 width, i32 height, Color);

    // Mixes `color` over the pixel; `coverage` runs from 0 (none) to 256 (opaque).
    void blend_pixel(i32 x, i32 y, Color, u32 coverage);

    // Moves the picture up by `rows`, filling the rows freed at the bottom.
    void scroll_up(u32 rows, Color fill);

private:
    [[nodiscard]] u32* row(u32 y) const { return reinterpret_cast<u32*>(m_base + u64(y) * m_pitch); }
    [[nodiscard]] u32 pack(Color) const;
    [[nodiscard]] Color unpack(u32) const;

    u8* m_base { nullptr };
    u32 m_width { 0 };
    u32 m_height { 0 };
    u32 m_pitch { 0 };
    API::PixelFormat m_format { API::PixelFormat::None };
};

Framebuffer& boot_framebuffer();

}
