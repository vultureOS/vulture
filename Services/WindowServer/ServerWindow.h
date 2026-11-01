/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/RefPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibWindow/Protocol.h>

namespace WindowServer {

struct Client;
struct WindowShape;

struct MaterialRegion {
    Gfx::IntRect rect; // Points, from the window's top-left.
    Window::Material material { Window::Material::Sidebar };
};

// A traffic light.
enum class WindowButton : u8 {
    None,
    Close,
    Minimize,
    Zoom,
};

struct ServerWindow {
    u32 id { 0 };
    Client* client { nullptr };
    String title;
    Gfx::IntRect frame; // Points.
    u32 flags { 0 };
    bool visible { false };
    bool active { false };
    int level { Window::LevelNormal };
    WindowShape const* shape { nullptr };
    Window::CursorShape cursor { Window::CursorShape::Arrow };
    // In points; 0 for the theme's.
    int title_bar_override { 0 };
    bool accepts_drop { false };

    // The client's memory, mapped here, and the two buffers in it.
    void* memory { nullptr };
    size_t memory_size { 0 };
    RefPtr<Gfx::Bitmap> buffers[2];
    int shown_buffer { 0 };
    bool has_presented { false };
    bool present_pending { false };

    Vector<MaterialRegion> materials;

    // The title bar, drawn over the client's pixels.
    RefPtr<Gfx::Bitmap> chrome;
    bool chrome_is_stale { true };
    bool lights_hovered { false };
    WindowButton pressed_button { WindowButton::None };

    [[nodiscard]] bool is_bare() const { return flags & Window::WindowBare; }
    [[nodiscard]] bool is_titled() const { return flags & Window::WindowTitled; }
    [[nodiscard]] bool is_resizable() const { return flags & Window::WindowResizable; }
    [[nodiscard]] bool is_popup() const { return flags & Window::WindowPopup; }
    [[nodiscard]] bool is_unified() const { return flags & Window::WindowUnifiedTitle; }
    [[nodiscard]] Gfx::Bitmap const* shown_surface() const { return has_presented ? buffers[shown_buffer].ptr() : nullptr; }
};

}
