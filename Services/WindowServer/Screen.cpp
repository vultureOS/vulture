/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Screen.h"
#include "WorkerPool.h"
#include <Kernel/API/POSIX/graphics.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <LibBase/StdLibExtras.h>

namespace WindowServer {

using u32x4 = u32 __attribute__((vector_size(16)));
using u8x4 = u8 __attribute__((vector_size(4)));

namespace {

[[gnu::always_inline]] inline u32x4 widened(Gfx::Pixel pixel)
{
    u8x4 narrow;
    __builtin_memcpy(&narrow, &pixel, 4);
    return __builtin_convertvector(narrow, u32x4);
}

[[gnu::always_inline]] inline Gfx::Pixel narrowed(u32x4 wide)
{
    u8x4 narrow = __builtin_convertvector(wide, u8x4);
    Gfx::Pixel pixel;
    __builtin_memcpy(&pixel, &narrow, 4);
    return pixel;
}

}

Screen::~Screen()
{
    for (auto& output : m_outputs) {
        output.panel = nullptr;
        munmap(output.mapping, output.mapping_size);
        close(output.fd);
    }
}

ErrorOr<NonnullOwnPtr<Screen>> Screen::open()
{
    auto screen = adopt_own(*new Screen);
    // /dev/gpu0, /dev/gpu1 and so on: as many as there are.
    for (int number = 0; number < 8; ++number) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/gpu%d", number);
        int fd = ::open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            if (number == 0)
                return Error::from_errno(errno);
            break;
        }
        vulture_gpu_mode mode {};
        if (ioctl(fd, VULTURE_GPU_GET_DISPLAY, &mode) < 0 || ioctl(fd, VULTURE_GPU_SET_MODE, &mode) < 0) {
            int const error = errno;
            close(fd);
            return Error::from_errno(error);
        }
        void* mapping = mmap(nullptr, mode.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapping == MAP_FAILED) {
            int const error = errno;
            close(fd);
            return Error::from_errno(error);
        }
        Output output;
        output.fd = fd;
        output.mapping = mapping;
        output.mapping_size = mode.size;
        output.panel = TRY(Gfx::Bitmap::wrap(static_cast<Gfx::Pixel*>(mapping), { static_cast<int>(mode.width), static_cast<int>(mode.height) }, static_cast<int>(mode.pitch / sizeof(Gfx::Pixel))));
        if (number == 0) {
            // A HiDPI display is one with room for it: macOS's 2x default. All of them go by the first.
            screen->m_scale = mode.width >= 2000 ? 2 : 1;
        }
        output.points = { static_cast<int>(mode.width) / screen->m_scale, static_cast<int>(mode.height) / screen->m_scale };
        // In a row to start with, to the right of the first.
        if (number > 0) {
            auto const& before = screen->m_outputs.last();
            output.origin = { before.origin.x + before.points.width, 0 };
        }
        screen->m_outputs.append(move(output));
    }
    TRY(screen->rebuild());
    return screen;
}

ErrorOr<void> Screen::rebuild()
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool first = true;
    for (auto const& output : m_outputs) {
        if (m_mirrored && &output != &m_outputs[0])
            continue;
        left = first ? output.origin.x : min(left, output.origin.x);
        top = first ? output.origin.y : min(top, output.origin.y);
        right = first ? output.origin.x + output.points.width : max(right, output.origin.x + output.points.width);
        bottom = first ? output.origin.y + output.points.height : max(bottom, output.origin.y + output.points.height);
        first = false;
    }
    m_origin = { -left, -top };
    Gfx::IntSize const size { (right - left) * m_scale, (bottom - top) * m_scale };

    m_displays.clear();
    for (size_t i = 0; i < m_outputs.size(); ++i) {
        auto& output = m_outputs[i];
        bool const shows_the_first = m_mirrored && i > 0;
        auto const origin = shows_the_first ? m_outputs[0].origin : output.origin;
        auto const points = shows_the_first ? m_outputs[0].points : output.points;
        output.area = { (origin.x + m_origin.x) * m_scale, (origin.y + m_origin.y) * m_scale, points.width * m_scale, points.height * m_scale };
        if (!shows_the_first)
            m_displays.append(Display { static_cast<int>(i), { output.origin.x, output.origin.y, output.points.width, output.points.height }, output.panel->size() });
    }

    bool const one_to_one = m_outputs.size() == 1 && m_warmth <= 0 && size == m_outputs[0].panel->size();
    if (one_to_one) {
        m_framebuffer = m_outputs[0].panel;
        m_direct = true;
        return {};
    }
    if (m_direct || !m_framebuffer || m_framebuffer->size() != size)
        m_framebuffer = TRY(Gfx::Bitmap::create(size));
    m_direct = false;
    return {};
}

ErrorOr<void> Screen::arrange(Span<Placement const> placements, bool mirrored)
{
    for (auto const& placement : placements) {
        if (placement.output < 0 || static_cast<size_t>(placement.output) >= m_outputs.size())
            return Error::from_errno(EINVAL);
        if (placement.points.width < 320 || placement.points.height < 240 || placement.points.width > 8192 || placement.points.height > 8192)
            return Error::from_errno(EINVAL);
    }
    for (auto const& placement : placements) {
        auto& output = m_outputs[static_cast<size_t>(placement.output)];
        output.origin = placement.origin;
        output.points = placement.points;
    }
    // The main one's where everything else is measured from.
    auto const main = m_outputs[0].origin;
    for (auto& output : m_outputs)
        output.origin = { output.origin.x - main.x, output.origin.y - main.y };
    m_mirrored = mirrored && m_outputs.size() > 1;
    return rebuild();
}

ErrorOr<void> Screen::set_warmth(float warmth)
{
    m_warmth = clamp(warmth, 0.0f, 1.0f);
    // Taking away blue, and less green, as a low sun does: the colors of about 2700 K at the most.
    float const green = 1 - 0.22f * m_warmth;
    float const blue = 1 - 0.55f * m_warmth;
    for (int i = 0; i < 256; ++i) {
        m_warm_red[i] = static_cast<u8>(i);
        m_warm_green[i] = static_cast<u8>(static_cast<float>(i) * green + 0.5f);
        m_warm_blue[i] = static_cast<u8>(static_cast<float>(i) * blue + 0.5f);
    }
    return rebuild();
}

Display const& Screen::display_at(Gfx::IntPoint point) const
{
    Display const* nearest = &m_displays[0];
    long nearest_distance = -1;
    for (auto const& display : m_displays) {
        if (display.points.contains(point))
            return display;
        long const across = point.x < display.points.x ? display.points.x - point.x : (point.x >= display.points.right() ? point.x - display.points.right() + 1 : 0);
        long const down = point.y < display.points.y ? display.points.y - point.y : (point.y >= display.points.bottom() ? point.y - display.points.bottom() + 1 : 0);
        long const distance = across * across + down * down;
        if (nearest_distance < 0 || distance < nearest_distance) {
            nearest = &display;
            nearest_distance = distance;
        }
    }
    return *nearest;
}

Display const& Screen::display_of_head(int head) const
{
    for (auto const& display : m_displays) {
        if (display.output == head)
            return display;
    }
    return m_displays[0];
}

Gfx::IntPoint Screen::nearest_point_on_a_display(Gfx::IntPoint point) const
{
    auto const& display = display_at(point);
    return { clamp(point.x, display.points.x, display.points.right() - 1), clamp(point.y, display.points.y, display.points.bottom() - 1) };
}

void Screen::copy_rows(Output const& output, int first_row, int end_row, Gfx::IntRect damage) const
{
    auto& panel = const_cast<Gfx::Bitmap&>(*output.panel);
    auto const& area = output.area;
    auto const& picture = *m_framebuffer;
    bool const one_to_one = area.size() == panel.size();
    bool const warm = m_warmth > 0;
    int const left = one_to_one ? damage.x - area.x : (damage.x - area.x) * panel.width() / area.width;
    int const right = one_to_one ? damage.right() - area.x : min(((damage.right() - area.x) * panel.width() + area.width - 1) / area.width + 1, panel.width());
    for (int y = first_row; y < end_row; ++y) {
        auto* out = panel.scanline(y);
        if (one_to_one) {
            __builtin_memcpy(out + left, picture.scanline(area.y + y) + area.x + left, static_cast<size_t>(right - left) * sizeof(Gfx::Pixel));
        } else {
            // Bilinear: a Mac draws "looks like" at twice the points and scales it to the panel, and so does this.
            u32 const row_position = static_cast<u32>((y * 2 + 1) * area.height * 128 / panel.height());
            int const upper = max(0, static_cast<int>(row_position / 256) - (row_position % 256 < 128 ? 1 : 0));
            int const lower = min(upper + 1, area.height - 1);
            u32 const down = (row_position + 128) % 256;
            auto const* top_row = picture.scanline(area.y + upper) + area.x;
            auto const* bottom_row = picture.scanline(area.y + lower) + area.x;
            for (int x = left; x < right; ++x) {
                u32 const position = static_cast<u32>((x * 2 + 1) * area.width * 128 / panel.width());
                int const before = max(0, static_cast<int>(position / 256) - (position % 256 < 128 ? 1 : 0));
                int const after = min(before + 1, area.width - 1);
                u32 const along = (position + 128) % 256;
                u32x4 const upper_mix = widened(top_row[before]) * (256 - along) + widened(top_row[after]) * along;
                u32x4 const lower_mix = widened(bottom_row[before]) * (256 - along) + widened(bottom_row[after]) * along;
                out[x] = narrowed((upper_mix * (256 - down) + lower_mix * down + 32768u) >> 16);
            }
        }
        if (warm) {
            for (int x = left; x < right; ++x) {
                Gfx::Pixel const pixel = out[x];
                out[x] = Gfx::make_pixel(m_warm_red[Gfx::pixel_red(pixel)], m_warm_green[Gfx::pixel_green(pixel)], m_warm_blue[Gfx::pixel_blue(pixel)], Gfx::pixel_alpha(pixel));
            }
        }
    }
}

void Screen::flush_output(Output const& output, Gfx::IntRect damage, WorkerPool& workers)
{
    auto const part = damage.intersected(output.area);
    if (part.is_empty())
        return;
    auto const& panel = *output.panel;
    int const first_row = (part.y - output.area.y) * panel.height() / output.area.height;
    int const end_row = min(((part.bottom() - output.area.y) * panel.height() + output.area.height - 1) / output.area.height + 1, panel.height());
    struct Job {
        Screen const* screen;
        Output const* output;
        int first_row;
        int end_row;
        Gfx::IntRect damage;
    } job { this, &output, first_row, end_row, part };
    int const band = 64;
    workers.run(static_cast<size_t>((end_row - first_row + band - 1) / band), [](void* context, size_t index) {
        auto const& work = *static_cast<Job const*>(context);
        int const from = work.first_row + static_cast<int>(index) * band;
        work.screen->copy_rows(*work.output, from, min(from + band, work.end_row), work.damage);
    },
        &job);
    // Whole rows. QEMU copies those in one go, and anything narrower a row at a time, starting from the first page of the
    // framebuffer for each: most of a full-screen flush's time for a small part of the screen.
    vulture_gpu_rect rows { 0, static_cast<unsigned>(first_row), static_cast<unsigned>(panel.width()), static_cast<unsigned>(end_row - first_row) };
    if (ioctl(output.fd, VULTURE_GPU_FLUSH, &rows) < 0)
        fprintf(stderr, "WindowServer: flush: %s\n", strerror(errno));
}

void Screen::flush(Gfx::IntRect rect, WorkerPool& workers)
{
    if (m_direct) {
        auto const& output = m_outputs[0];
        vulture_gpu_rect rows { 0, static_cast<unsigned>(rect.y), static_cast<unsigned>(m_framebuffer->width()), static_cast<unsigned>(rect.height) };
        if (ioctl(output.fd, VULTURE_GPU_FLUSH, &rows) < 0)
            fprintf(stderr, "WindowServer: flush: %s\n", strerror(errno));
        return;
    }
    for (auto const& output : m_outputs)
        flush_output(output, rect, workers);
}

Screen::Output const* Screen::output_at(Gfx::IntPoint pixel) const
{
    for (auto const& output : m_outputs) {
        if (output.area.contains(pixel))
            return &output;
    }
    return nullptr;
}

void Screen::set_cursor(Gfx::Bitmap const& image, Gfx::IntPoint hot_spot)
{
    size_t const count = VULTURE_GPU_CURSOR_SIZE * VULTURE_GPU_CURSOR_SIZE;
    if (m_cursor_pixels.size() != count)
        MUST(m_cursor_pixels.try_resize(count));
    __builtin_memset(m_cursor_pixels.data(), 0, count * sizeof(Gfx::Pixel));
    for (int y = 0; y < min(image.height(), VULTURE_GPU_CURSOR_SIZE); ++y)
        __builtin_memcpy(m_cursor_pixels.data() + y * VULTURE_GPU_CURSOR_SIZE, image.scanline(y), static_cast<size_t>(min(image.width(), VULTURE_GPU_CURSOR_SIZE)) * sizeof(Gfx::Pixel));
    m_cursor_hot_spot = hot_spot;
    vulture_gpu_cursor cursor { m_cursor_pixels.data(), static_cast<unsigned>(hot_spot.x), static_cast<unsigned>(hot_spot.y) };
    if (ioctl(m_outputs[static_cast<size_t>(m_cursor_output)].fd, VULTURE_GPU_SET_CURSOR, &cursor) < 0)
        fprintf(stderr, "WindowServer: set cursor: %s\n", strerror(errno));
}

void Screen::move_cursor(Gfx::IntPoint pixel)
{
    auto const* now_on = output_at(pixel);
    if (!now_on)
        now_on = &m_outputs[static_cast<size_t>(m_cursor_output)];
    int const index = static_cast<int>(now_on - m_outputs.data());
    if (index != m_cursor_output) {
        // A display has a cursor of its own: this one's gone from the last, and it has to be given the picture.
        (void)ioctl(m_outputs[static_cast<size_t>(m_cursor_output)].fd, VULTURE_GPU_HIDE_CURSOR, nullptr);
        m_cursor_output = index;
        if (!m_cursor_pixels.is_empty()) {
            vulture_gpu_cursor cursor { m_cursor_pixels.data(), static_cast<unsigned>(m_cursor_hot_spot.x), static_cast<unsigned>(m_cursor_hot_spot.y) };
            (void)ioctl(now_on->fd, VULTURE_GPU_SET_CURSOR, &cursor);
        }
    }
    auto const& panel = *now_on->panel;
    int const x = (pixel.x - now_on->area.x) * panel.width() / max(now_on->area.width, 1);
    int const y = (pixel.y - now_on->area.y) * panel.height() / max(now_on->area.height, 1);
    vulture_gpu_position position { static_cast<unsigned>(max(x, 0)), static_cast<unsigned>(max(y, 0)) };
    (void)ioctl(now_on->fd, VULTURE_GPU_MOVE_CURSOR, &position);
}

}
