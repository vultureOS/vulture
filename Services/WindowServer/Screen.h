/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>
#include <LibGfx/Bitmap.h>

namespace WindowServer {

// The display: its framebuffer (which the compositor draws in directly: the
// device only looks when told) and its cursor.
class Screen {
public:
    [[nodiscard]] static ErrorOr<NonnullOwnPtr<Screen>> open(char const* device);
    ~Screen();

    [[nodiscard]] Gfx::Bitmap& framebuffer() { return *m_framebuffer; }
    [[nodiscard]] Gfx::IntSize pixels() const { return m_framebuffer->size(); }
    [[nodiscard]] int scale() const { return m_scale; }
    [[nodiscard]] Gfx::IntSize points() const { return { pixels().width / m_scale, pixels().height / m_scale }; }

    void flush(Gfx::IntRect);
    void set_cursor(Gfx::Bitmap const&, Gfx::IntPoint hot_spot);
    void move_cursor(Gfx::IntPoint pixel);

private:
    Screen(int fd, NonnullRefPtr<Gfx::Bitmap>, int scale, void* mapping, size_t size);

    int m_fd;
    NonnullRefPtr<Gfx::Bitmap> m_framebuffer;
    int m_scale;
    void* m_mapping;
    size_t m_mapping_size;
};

}
