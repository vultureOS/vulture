/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/graphics.h>
#include <Kernel/Console/Framebuffer.h>
#include <Kernel/Console/TextConsole.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Graphics/Graphics.h>
#include <Kernel/FileSystem/DevFS.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Memory/UserAccess.h>
#include <LibBase/New.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

// What there is when there's no driver for the machine's graphics: the framebuffer the firmware left
// on, at the resolution it chose. That is all a machine needs for this system to draw on, as drawing is
// done by the processor anyway. It's slower to read than memory, so what's drawn is drawn in a copy in
// memory, and flushing copies the part that changed. There's no hardware cursor either: the pointer is
// mixed in with what's being copied.

namespace Kernel {

void initialize_firmware_display();

namespace {

class FirmwareDisplay final : public Device {
public:
    FirmwareDisplay()
        : Device(InodeType::CharacterDevice, 0600, device_number(29, 0))
    {
    }

    ErrorOr<size_t> read(u64, size_t, IOBuffer const&) override { return Error::from_errno(EINVAL); }
    ErrorOr<size_t> write(u64, size_t, IOBuffer const&) override { return Error::from_errno(EINVAL); }
    [[nodiscard]] bool can_be_mapped() const override { return true; }

    ErrorOr<PhysicalAddress> mapped_page(u64 index) override
    {
        MutexLocker locker(m_lock);
        if (index >= m_pages.size())
            return Error::from_errno(ENXIO);
        physical_memory().ref_user_page(m_pages[index]);
        return m_pages[index];
    }

    ErrorOr<FlatPtr> ioctl(FileDescription&, unsigned request, FlatPtr argument) override
    {
        auto const& screen = boot_framebuffer();
        switch (request) {
        case VULTURE_GPU_GET_DISPLAY: {
            vulture_gpu_mode mode {};
            mode.width = screen.width();
            mode.height = screen.height();
            mode.pitch = mode.width * 4;
            mode.size = static_cast<unsigned long>(mode.pitch) * mode.height;
            TRY(copy_to_user(argument, mode));
            return 0;
        }
        case VULTURE_GPU_SET_MODE: {
            auto mode = TRY(copy_from_user<vulture_gpu_mode>(argument));
            // It has the one mode, which is what it was left in.
            if (mode.width != screen.width() || mode.height != screen.height())
                return Error::from_errno(EINVAL);
            TRY(allocate_shadow());
            mode.pitch = mode.width * 4;
            mode.size = static_cast<unsigned long>(mode.pitch) * mode.height;
            TRY(copy_to_user(argument, mode));
            return 0;
        }
        case VULTURE_GPU_FLUSH: {
            auto const rect = TRY(copy_from_user<vulture_gpu_rect>(argument));
            MutexLocker locker(m_lock);
            if (m_pages.is_empty())
                return Error::from_errno(EINVAL);
            present(rect);
            return 0;
        }
        case VULTURE_GPU_SET_CURSOR: {
            auto const cursor = TRY(copy_from_user<vulture_gpu_cursor>(argument));
            if (cursor.hot_x >= VULTURE_GPU_CURSOR_SIZE || cursor.hot_y >= VULTURE_GPU_CURSOR_SIZE)
                return Error::from_errno(EINVAL);
            static u32 incoming[VULTURE_GPU_CURSOR_SIZE * VULTURE_GPU_CURSOR_SIZE];
            MutexLocker locker(m_lock);
            TRY(copy_from_user(incoming, reinterpret_cast<FlatPtr>(cursor.pixels), sizeof(incoming)));
            __builtin_memcpy(m_cursor, incoming, sizeof(m_cursor));
            auto const before = cursor_rect();
            m_hot_x = cursor.hot_x;
            m_hot_y = cursor.hot_y;
            m_has_cursor = true;
            present(before);
            present(cursor_rect());
            return 0;
        }
        case VULTURE_GPU_MOVE_CURSOR: {
            auto const position = TRY(copy_from_user<vulture_gpu_position>(argument));
            MutexLocker locker(m_lock);
            auto const before = cursor_rect();
            m_cursor_x = position.x;
            m_cursor_y = position.y;
            present(before);
            present(cursor_rect());
            return 0;
        }
        default:
            return Error::from_errno(ENOTTY);
        }
    }

private:
    ErrorOr<void> allocate_shadow()
    {
        MutexLocker locker(m_lock);
        auto const& screen = boot_framebuffer();
        size_t const bytes = static_cast<size_t>(screen.width()) * screen.height() * 4;
        size_t const pages = align_up<size_t>(bytes, page_size) / page_size;
        if (m_pages.size() == pages)
            return {};
        for (size_t i = 0; i < pages; ++i)
            TRY(m_pages.try_append(TRY(physical_memory().allocate_user_page())));
        // From now on, the screen belongs to whoever opened this. (The console would write over it.)
        boot_text_console().set_suspended(true);
        return {};
    }

    // Where the corner of the pointer's picture is: off the screen, when its hot spot is near the top or left.
    [[nodiscard]] i32 cursor_left() const { return static_cast<i32>(m_cursor_x) - static_cast<i32>(m_hot_x); }
    [[nodiscard]] i32 cursor_top() const { return static_cast<i32>(m_cursor_y) - static_cast<i32>(m_hot_y); }

    // What it covers on the screen.
    [[nodiscard]] vulture_gpu_rect cursor_rect() const
    {
        i32 const size = VULTURE_GPU_CURSOR_SIZE;
        i32 const left = max(cursor_left(), 0);
        i32 const top = max(cursor_top(), 0);
        return { static_cast<u32>(left), static_cast<u32>(top), static_cast<u32>(cursor_left() + size - left), static_cast<u32>(cursor_top() + size - top) };
    }

    [[nodiscard]] u32 const* shadow_at(size_t offset) const
    {
        return reinterpret_cast<u32 const*>(physical_to_virtual(m_pages[offset / page_size]) + offset % page_size);
    }

    // What's in the copy, then the pointer on top, for this much of the screen.
    void present(vulture_gpu_rect rect)
    {
        auto& screen = boot_framebuffer();
        if (m_pages.is_empty() || rect.x >= screen.width() || rect.y >= screen.height())
            return;
        u32 const right = min(rect.x + rect.width, screen.width());
        u32 const bottom = min(rect.y + rect.height, screen.height());
        size_t const stride = static_cast<size_t>(screen.width()) * 4;
        auto const cursor = cursor_rect();
        for (u32 y = rect.y; y < bottom; ++y) {
            u32 x = rect.x;
            auto* target = screen.raw_row(y);
            while (x < right) {
                // As far as the end of this page of the copy.
                size_t const offset = y * stride + static_cast<size_t>(x) * 4;
                size_t const in_page = (page_size - offset % page_size) / 4;
                u32 const count = min<u32>(right - x, static_cast<u32>(in_page));
                auto const* source = shadow_at(offset);
                if (screen.has_native_byte_order()) {
                    __builtin_memcpy(target + x, source, count * 4);
                } else {
                    for (u32 i = 0; i < count; ++i)
                        target[x + i] = swap_red_and_blue(source[i]);
                }
                x += count;
            }
            // The pointer, where it's in this row.
            if (m_has_cursor && y >= cursor.y && y < cursor.y + cursor.height) {
                u32 const first = max(rect.x, cursor.x);
                u32 const last = min(right, cursor.x + cursor.width);
                u32 const cursor_row = static_cast<u32>(static_cast<i32>(y) - cursor_top()) * VULTURE_GPU_CURSOR_SIZE;
                for (u32 at = first; at < last; ++at) {
                    u32 const pixel = m_cursor[cursor_row + static_cast<u32>(static_cast<i32>(at) - cursor_left())];
                    u32 const alpha = pixel >> 24;
                    if (alpha == 0)
                        continue;
                    size_t const offset = y * stride + static_cast<size_t>(at) * 4;
                    u32 const under = *shadow_at(offset);
                    // Premultiplied.
                    u32 const inverse = 255 - alpha;
                    u32 const blue = (pixel & 0xff) + (under & 0xff) * inverse / 255;
                    u32 const green = ((pixel >> 8) & 0xff) + ((under >> 8) & 0xff) * inverse / 255;
                    u32 const red = ((pixel >> 16) & 0xff) + ((under >> 16) & 0xff) * inverse / 255;
                    u32 const mixed = blue | (green << 8) | (red << 16);
                    target[at] = screen.has_native_byte_order() ? mixed : swap_red_and_blue(mixed);
                }
            }
        }
    }

    [[nodiscard]] static u32 swap_red_and_blue(u32 pixel)
    {
        return (pixel & 0xff00ff00) | ((pixel & 0xff) << 16) | ((pixel >> 16) & 0xff);
    }

    Mutex m_lock;
    Vector<PhysicalAddress> m_pages;
    u32 m_cursor[VULTURE_GPU_CURSOR_SIZE * VULTURE_GPU_CURSOR_SIZE] {};
    u32 m_cursor_x { 0 };
    u32 m_cursor_y { 0 };
    u32 m_hot_x { 0 };
    u32 m_hot_y { 0 };
    bool m_has_cursor { false };
};

}

void initialize_firmware_display()
{
    auto const& screen = boot_framebuffer();
    if (!screen.is_present()) {
        dmesgln("Graphics: no display");
        return;
    }
    auto* device = new (Base::nothrow) FirmwareDisplay;
    if (!device)
        return;
    dmesgln("Graphics: gpu0: the firmware's framebuffer, {}x{}", screen.width(), screen.height());
    if (auto added = DevFS::the().add_device("gpu0"_sv, adopt_ref(*static_cast<Inode*>(device))); added.is_error())
        dmesgln("Graphics: gpu0: {}", added.error());
}

}
