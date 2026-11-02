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

// The linear framebuffer the firmware set up. Drawing is clipped to the
// screen.
class Framebuffer {
public:
    // Starts out reached through the loader's direct map.
    void initialize(API::FramebufferInfo const&);

    // Points at a new mapping of the same pixels.
    void relocate(FlatPtr pixels) { m_base = reinterpret_cast<u8*>(pixels); }

    [[nodiscard]] bool is_present() const { return m_base != nullptr; }
    [[nodiscard]] u32 width() const { return m_width; }
    [[nodiscard]] u32 height() const { return m_height; }
    // For copying a whole picture to it, which a pixel at a time would take too long for.
    [[nodiscard]] u32* raw_row(u32 y) const { return row(y); }
    [[nodiscard]] bool has_native_byte_order() const { return m_format == API::PixelFormat::BGRX8888; }

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
