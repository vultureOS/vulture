/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/graphics.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Graphics/Graphics.h>
#include <Kernel/Devices/VirtIO/VirtIO.h>
#include <Kernel/FileSystem/DevFS.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Memory/UserAccess.h>
#include <LibBase/New.h>

// virtio-gpu in its 2D mode: the framebuffer is ordinary memory the device
// copies from when told what changed. The cursor is a small resource of its own
// on a plane of its own, so moving it redraws nothing.

namespace Kernel {

namespace {

constexpr u16 device_id = 0x1050;

constexpr u32 command_get_display_info = 0x0100;
constexpr u32 command_resource_create_2d = 0x0101;
constexpr u32 command_resource_unref = 0x0102;
constexpr u32 command_set_scanout = 0x0103;
constexpr u32 command_resource_flush = 0x0104;
constexpr u32 command_transfer_to_host_2d = 0x0105;
constexpr u32 command_resource_attach_backing = 0x0106;
constexpr u32 command_resource_detach_backing = 0x0107;
constexpr u32 command_update_cursor = 0x0300;
constexpr u32 command_move_cursor = 0x0301;

constexpr u32 response_ok_nodata = 0x1100;
constexpr u32 response_ok_display_info = 0x1101;

constexpr u32 format_b8g8r8a8 = 1;
constexpr u32 format_b8g8r8x8 = 2;

struct Header {
    u32 type;
    u32 flags;
    u64 fence_id;
    u32 context_id;
    u32 padding;
};
static_assert(sizeof(Header) == 24);

struct Rectangle {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
};

struct DisplayInfo {
    Header header;
    struct {
        Rectangle rectangle;
        u32 enabled;
        u32 flags;
    } modes[16];
};

struct ResourceCreate2D {
    Header header;
    u32 resource_id;
    u32 format;
    u32 width;
    u32 height;
};

struct ResourceId {
    Header header;
    u32 resource_id;
    u32 padding;
};

struct SetScanout {
    Header header;
    Rectangle rectangle;
    u32 scanout_id;
    u32 resource_id;
};

struct ResourceFlush {
    Header header;
    Rectangle rectangle;
    u32 resource_id;
    u32 padding;
};

struct TransferToHost2D {
    Header header;
    Rectangle rectangle;
    u64 offset;
    u32 resource_id;
    u32 padding;
};

struct AttachBacking {
    Header header;
    u32 resource_id;
    u32 entry_count;
};

struct BackingEntry {
    u64 address;
    u32 length;
    u32 padding;
};

struct UpdateCursor {
    Header header;
    u32 scanout_id;
    u32 x;
    u32 y;
    u32 padding;
    u32 resource_id;
    u32 hot_x;
    u32 hot_y;
    u32 padding2;
};

constexpr u32 cursor_bytes = VULTURE_GPU_CURSOR_SIZE * VULTURE_GPU_CURSOR_SIZE * 4;
constexpr unsigned cursor_order = 3; // 32 KiB: more than 64 x 64 x 4.
constexpr u32 scanout = 0;

class VirtIOGPU final : public Device {
public:
    static ErrorOr<NonnullRefPtr<VirtIOGPU>> create(PCI::Device const& pci)
    {
        auto transport = TRY(try_make<VirtIO::Device>(pci));
        TRY(transport->initialize(0));
        auto control_queue = TRY(transport->setup_queue(0, 64));
        auto cursor_queue = TRY(transport->setup_queue(1, 16));
        auto control = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
        auto* device = new (Base::nothrow) VirtIOGPU(move(transport), move(control_queue), move(cursor_queue), control);
        if (!device)
            return Error::from_errno(ENOMEM);
        auto reference = adopt_ref(*device);
        auto interrupt = pci.interrupt();
        if (!interrupt.has_value())
            return Error::from_errno(ENODEV);
        register_interrupt_handler(interrupt.value(), [](void* context) { static_cast<VirtIOGPU*>(context)->handle_interrupt(); }, device);
        Arch::enable_interrupt(interrupt.value());
        reference->m_transport->finish_initialization();
        TRY(reference->query_display());
        return reference;
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
        switch (request) {
        case VULTURE_GPU_GET_DISPLAY: {
            vulture_gpu_mode mode {};
            mode.width = m_display_width;
            mode.height = m_display_height;
            mode.pitch = m_display_width * 4;
            mode.size = static_cast<unsigned long>(mode.pitch) * mode.height;
            TRY(copy_to_user(argument, mode));
            return 0;
        }
        case VULTURE_GPU_SET_MODE: {
            auto mode = TRY(copy_from_user<vulture_gpu_mode>(argument));
            TRY(set_mode(mode.width, mode.height));
            mode.pitch = mode.width * 4;
            mode.size = static_cast<unsigned long>(mode.pitch) * mode.height;
            TRY(copy_to_user(argument, mode));
            return 0;
        }
        case VULTURE_GPU_FLUSH:
            TRY(flush(TRY(copy_from_user<vulture_gpu_rect>(argument))));
            return 0;
        case VULTURE_GPU_SET_CURSOR: {
            auto cursor = TRY(copy_from_user<vulture_gpu_cursor>(argument));
            if (cursor.hot_x >= VULTURE_GPU_CURSOR_SIZE || cursor.hot_y >= VULTURE_GPU_CURSOR_SIZE)
                return Error::from_errno(EINVAL);
            TRY(set_cursor(reinterpret_cast<FlatPtr>(cursor.pixels), cursor.hot_x, cursor.hot_y));
            return 0;
        }
        case VULTURE_GPU_MOVE_CURSOR: {
            auto position = TRY(copy_from_user<vulture_gpu_position>(argument));
            TRY(move_cursor(position.x, position.y));
            return 0;
        }
        default:
            return Error::from_errno(ENOTTY);
        }
    }

    [[nodiscard]] u32 display_width() const { return m_display_width; }
    [[nodiscard]] u32 display_height() const { return m_display_height; }

private:
    VirtIOGPU(NonnullOwnPtr<VirtIO::Device> transport, NonnullOwnPtr<VirtIO::Queue> control_queue, NonnullOwnPtr<VirtIO::Queue> cursor_queue, PhysicalAddress control)
        : Device(InodeType::CharacterDevice, 0600, device_number(29, 0))
        , m_transport(move(transport))
        , m_control_queue(move(control_queue))
        , m_cursor_queue(move(cursor_queue))
        , m_control(control)
    {
    }

    // Sends a request (in the control page, unless it brought its own memory)
    // and waits for the response, which is put after it there.
    ErrorOr<void> command(VirtIO::Queue& queue, PhysicalAddress request, size_t request_size, size_t response_size, u32 expected)
    {
        auto response = m_control.offset(page_size / 2);
        auto* header = reinterpret_cast<Header*>(physical_to_virtual(response));
        VERIFY(response_size <= page_size / 2);
        __builtin_memset(header, 0, response_size);
        VirtIO::Buffer const buffers[2] = { { request, static_cast<u32>(request_size), false }, { response, static_cast<u32>(response_size), true } };
        m_done.store(false);
        TRY(queue.submit({ buffers, 2 }));
        m_transport->notify(queue);
        m_completed.wait_until([&] { return m_done.load(); });
        if (header->type != expected) {
            dmesgln("virtio-gpu: command failed: response {:#x}", header->type);
            return Error::from_errno(EIO);
        }
        return {};
    }

    template<typename Request>
    ErrorOr<void> simple_command(Request const& request)
    {
        static_assert(sizeof(Request) <= page_size / 2);
        __builtin_memcpy(physical_to_virtual(m_control), &request, sizeof(Request));
        return command(*m_control_queue, m_control, sizeof(Request), sizeof(Header), response_ok_nodata);
    }

    // The cursor queue's commands have no response. Waits until it has been
    // taken, as the request's memory is about to be used again.
    ErrorOr<void> cursor_command(UpdateCursor const& request)
    {
        __builtin_memcpy(physical_to_virtual(m_control), &request, sizeof(request));
        VirtIO::Buffer const buffer { m_control, sizeof(request), false };
        m_done.store(false);
        TRY(m_cursor_queue->submit({ &buffer, 1 }));
        m_transport->notify(*m_cursor_queue);
        m_completed.wait_until([&] { return m_done.load(); });
        return {};
    }

    ErrorOr<void> query_display()
    {
        MutexLocker locker(m_lock);
        Header request {};
        request.type = command_get_display_info;
        __builtin_memcpy(physical_to_virtual(m_control), &request, sizeof(request));
        TRY(command(*m_control_queue, m_control, sizeof(request), sizeof(DisplayInfo), response_ok_display_info));
        auto const& info = *reinterpret_cast<DisplayInfo const*>(physical_to_virtual(m_control.offset(page_size / 2)));
        m_display_width = info.modes[scanout].rectangle.width;
        m_display_height = info.modes[scanout].rectangle.height;
        if (m_display_width == 0 || m_display_height == 0) {
            m_display_width = 1280;
            m_display_height = 800;
        }
        return {};
    }

    ErrorOr<void> attach_backing(u32 resource_id, Span<PhysicalAddress const> pages, size_t page_length)
    {
        // Runs of adjacent pages make one entry each. Up to one per page.
        size_t room = sizeof(AttachBacking) + pages.size() * sizeof(BackingEntry);
        unsigned order = 0;
        while ((page_size << order) < room)
            ++order;
        auto memory = TRY(physical_memory().allocate_pages(order, PageUse::General));
        auto* attach = reinterpret_cast<AttachBacking*>(physical_to_virtual(memory));
        auto* entries = reinterpret_cast<BackingEntry*>(attach + 1);
        u32 count = 0;
        for (size_t i = 0; i < pages.size(); ++i) {
            u64 address = pages[i].get();
            if (count > 0 && entries[count - 1].address + entries[count - 1].length == address) {
                entries[count - 1].length += static_cast<u32>(page_length);
            } else {
                entries[count++] = { address, static_cast<u32>(page_length), 0 };
            }
        }
        *attach = {};
        attach->header.type = command_resource_attach_backing;
        attach->resource_id = resource_id;
        attach->entry_count = count;
        auto result = command(*m_control_queue, memory, sizeof(AttachBacking) + count * sizeof(BackingEntry), sizeof(Header), response_ok_nodata);
        physical_memory().free_pages(memory);
        return result;
    }

    ErrorOr<void> set_mode(u32 width, u32 height)
    {
        if (width == 0 || height == 0 || width > 16384 || height > 16384)
            return Error::from_errno(EINVAL);
        MutexLocker locker(m_lock);
        size_t page_count = (static_cast<size_t>(width) * height * 4 + page_size - 1) / page_size;
        Vector<PhysicalAddress> pages;
        TRY(pages.try_ensure_capacity(page_count));
        auto release_pages = [&] {
            for (auto page : pages)
                physical_memory().unref_user_page(page);
        };
        for (size_t i = 0; i < page_count; ++i) {
            auto page = physical_memory().allocate_user_page();
            if (page.is_error()) {
                release_pages();
                return page.release_error();
            }
            pages.append(page.value());
        }
        u32 resource_id = m_next_resource_id++;
        ResourceCreate2D create {};
        create.header.type = command_resource_create_2d;
        create.resource_id = resource_id;
        create.format = format_b8g8r8x8;
        create.width = width;
        create.height = height;
        if (auto result = simple_command(create); result.is_error()) {
            release_pages();
            return result.release_error();
        }
        if (auto result = attach_backing(resource_id, pages.span(), page_size); result.is_error()) {
            release_pages();
            return result.release_error();
        }
        SetScanout set {};
        set.header.type = command_set_scanout;
        set.rectangle = { 0, 0, width, height };
        set.scanout_id = scanout;
        set.resource_id = resource_id;
        if (auto result = simple_command(set); result.is_error()) {
            release_pages();
            return result.release_error();
        }
        // The old one goes; its pages live as long as anyone maps them.
        if (m_resource_id) {
            ResourceId old {};
            old.header.type = command_resource_detach_backing;
            old.resource_id = m_resource_id;
            (void)simple_command(old);
            old.header.type = command_resource_unref;
            (void)simple_command(old);
            for (auto page : m_pages)
                physical_memory().unref_user_page(page);
        }
        m_pages = move(pages);
        m_resource_id = resource_id;
        m_width = width;
        m_height = height;
        return {};
    }

    ErrorOr<void> flush(vulture_gpu_rect rect)
    {
        MutexLocker locker(m_lock);
        if (!m_resource_id)
            return Error::from_errno(EINVAL);
        if (rect.x >= m_width || rect.y >= m_height || rect.width == 0 || rect.height == 0)
            return {};
        rect.width = min(rect.width, m_width - rect.x);
        rect.height = min(rect.height, m_height - rect.y);
        Rectangle area { rect.x, rect.y, rect.width, rect.height };
        TransferToHost2D transfer {};
        transfer.header.type = command_transfer_to_host_2d;
        transfer.rectangle = area;
        transfer.offset = static_cast<u64>(rect.y) * m_width * 4 + static_cast<u64>(rect.x) * 4;
        transfer.resource_id = m_resource_id;
        TRY(simple_command(transfer));
        ResourceFlush flush {};
        flush.header.type = command_resource_flush;
        flush.rectangle = area;
        flush.resource_id = m_resource_id;
        return simple_command(flush);
    }

    ErrorOr<void> set_cursor(FlatPtr pixels, u32 hot_x, u32 hot_y)
    {
        MutexLocker locker(m_lock);
        if (!m_cursor_resource_id) {
            m_cursor_memory = TRY(physical_memory().allocate_pages(cursor_order, PageUse::General));
            u32 resource_id = m_next_resource_id++;
            ResourceCreate2D create {};
            create.header.type = command_resource_create_2d;
            create.resource_id = resource_id;
            create.format = format_b8g8r8a8;
            create.width = VULTURE_GPU_CURSOR_SIZE;
            create.height = VULTURE_GPU_CURSOR_SIZE;
            TRY(simple_command(create));
            PhysicalAddress const backing[1] = { m_cursor_memory };
            TRY(attach_backing(resource_id, { backing, 1 }, cursor_bytes));
            m_cursor_resource_id = resource_id;
        }
        // Into kernel memory first: no user copy with the device waiting.
        TRY(copy_from_user(physical_to_virtual(m_cursor_memory), pixels, cursor_bytes));
        TransferToHost2D transfer {};
        transfer.header.type = command_transfer_to_host_2d;
        transfer.rectangle = { 0, 0, VULTURE_GPU_CURSOR_SIZE, VULTURE_GPU_CURSOR_SIZE };
        transfer.resource_id = m_cursor_resource_id;
        TRY(simple_command(transfer));
        UpdateCursor update {};
        update.header.type = command_update_cursor;
        update.scanout_id = scanout;
        update.x = m_cursor_x;
        update.y = m_cursor_y;
        update.resource_id = m_cursor_resource_id;
        update.hot_x = hot_x;
        update.hot_y = hot_y;
        return cursor_command(update);
    }

    ErrorOr<void> move_cursor(u32 x, u32 y)
    {
        MutexLocker locker(m_lock);
        m_cursor_x = x;
        m_cursor_y = y;
        UpdateCursor update {};
        update.header.type = command_move_cursor;
        update.scanout_id = scanout;
        update.x = x;
        update.y = y;
        return cursor_command(update);
    }

    void handle_interrupt()
    {
        if (!(m_transport->read_interrupt_status() & 1))
            return;
        bool any = false;
        while (m_control_queue->take_completion().has_value())
            any = true;
        while (m_cursor_queue->take_completion().has_value())
            any = true;
        if (any) {
            m_done.store(true);
            m_completed.wake_all();
        }
    }

    NonnullOwnPtr<VirtIO::Device> m_transport;
    NonnullOwnPtr<VirtIO::Queue> m_control_queue;
    NonnullOwnPtr<VirtIO::Queue> m_cursor_queue;
    PhysicalAddress m_control;
    Mutex m_lock;
    Atomic<bool> m_done { false };
    WaitQueue m_completed;

    u32 m_display_width { 0 };
    u32 m_display_height { 0 };
    u32 m_next_resource_id { 1 };
    u32 m_resource_id { 0 };
    u32 m_width { 0 };
    u32 m_height { 0 };
    Vector<PhysicalAddress> m_pages;
    u32 m_cursor_resource_id { 0 };
    PhysicalAddress m_cursor_memory;
    u32 m_cursor_x { 0 };
    u32 m_cursor_y { 0 };
};

}

void initialize_firmware_display();

void initialize_graphics()
{
    u32 number = 0;
    for (auto& pci : PCI::devices()) {
        if (pci.vendor_id() != VirtIO::vendor_id || pci.device_id() != device_id)
            continue;
        auto device = VirtIOGPU::create(pci);
        if (device.is_error()) {
            dmesgln("Graphics: virtio-gpu at {:02x}:{:02x}.{}: {}", pci.bus(), pci.slot(), pci.function(), device.error());
            continue;
        }
        StringBuilder name;
        name.appendff("gpu{}", number++);
        dmesgln("Graphics: {}: virtio-gpu, display {}x{}", name.view(), device.value()->display_width(), device.value()->display_height());
        if (auto added = DevFS::the().add_device(name.view(), NonnullRefPtr<Inode>(*device.value())); added.is_error())
            dmesgln("Graphics: {}: {}", name.view(), added.error());
    }
    // Without a driver for what's there, there's what the firmware left on.
    if (number == 0)
        initialize_firmware_display();
}

}
