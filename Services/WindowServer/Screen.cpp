/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Screen.h"
#include <Kernel/API/POSIX/graphics.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace WindowServer {

Screen::Screen(int fd, NonnullRefPtr<Gfx::Bitmap> framebuffer, int scale, void* mapping, size_t size)
    : m_fd(fd)
    , m_framebuffer(move(framebuffer))
    , m_scale(scale)
    , m_mapping(mapping)
    , m_mapping_size(size)
{
}

Screen::~Screen()
{
    munmap(m_mapping, m_mapping_size);
    close(m_fd);
}

ErrorOr<NonnullOwnPtr<Screen>> Screen::open(char const* device)
{
    int fd = ::open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    vulture_gpu_mode mode {};
    if (ioctl(fd, VULTURE_GPU_GET_DISPLAY, &mode) < 0 || ioctl(fd, VULTURE_GPU_SET_MODE, &mode) < 0) {
        int error = errno;
        close(fd);
        return Error::from_errno(error);
    }
    void* mapping = mmap(nullptr, mode.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        int error = errno;
        close(fd);
        return Error::from_errno(error);
    }
    auto bitmap = TRY(Gfx::Bitmap::wrap(static_cast<Gfx::Pixel*>(mapping), { static_cast<int>(mode.width), static_cast<int>(mode.height) }, static_cast<int>(mode.pitch / 4)));
    // A HiDPI display is one with room for it: macOS's 2x default.
    int scale = mode.width >= 2000 ? 2 : 1;
    auto* screen = new (Base::nothrow) Screen(fd, move(bitmap), scale, mapping, mode.size);
    if (!screen)
        return Error::from_errno(ENOMEM);
    return adopt_own(*screen);
}

void Screen::flush(Gfx::IntRect rect)
{
    // Whole rows. QEMU copies those in one go, and anything narrower a row at a time, starting from the first page of the
    // framebuffer for each: most of a full-screen flush's time for a small part of the screen.
    vulture_gpu_rect area { 0, static_cast<unsigned>(rect.y), static_cast<unsigned>(m_framebuffer->width()), static_cast<unsigned>(rect.height) };
    if (ioctl(m_fd, VULTURE_GPU_FLUSH, &area) < 0)
        fprintf(stderr, "WindowServer: flush: %s\n", strerror(errno));
}

void Screen::set_cursor(Gfx::Bitmap const& image, Gfx::IntPoint hot_spot)
{
    static Gfx::Pixel pixels[VULTURE_GPU_CURSOR_SIZE * VULTURE_GPU_CURSOR_SIZE];
    memset(pixels, 0, sizeof(pixels));
    for (int y = 0; y < min(image.height(), VULTURE_GPU_CURSOR_SIZE); ++y)
        memcpy(pixels + y * VULTURE_GPU_CURSOR_SIZE, image.scanline(y), static_cast<size_t>(min(image.width(), VULTURE_GPU_CURSOR_SIZE)) * sizeof(Gfx::Pixel));
    vulture_gpu_cursor cursor { pixels, static_cast<unsigned>(hot_spot.x), static_cast<unsigned>(hot_spot.y) };
    if (ioctl(m_fd, VULTURE_GPU_SET_CURSOR, &cursor) < 0)
        fprintf(stderr, "WindowServer: set cursor: %s\n", strerror(errno));
}

void Screen::move_cursor(Gfx::IntPoint pixel)
{
    vulture_gpu_position position { static_cast<unsigned>(max(pixel.x, 0)), static_cast<unsigned>(max(pixel.y, 0)) };
    (void)ioctl(m_fd, VULTURE_GPU_MOVE_CURSOR, &position);
}

}
