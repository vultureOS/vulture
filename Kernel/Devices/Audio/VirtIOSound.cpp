/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/audio.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Audio/Audio.h>
#include <Kernel/Devices/VirtIO/VirtIO.h>
#include <Kernel/FileSystem/DevFS.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Thread.h>
#include <LibBase/New.h>

// virtio-sound (virtio 1.2, section 5.14): one output stream, playing what is written to
// /dev/audio0. A write fills a period's buffer and queues it, as three buffers: which
// stream, the samples, and room for the device to say how it went. The device finishes
// with a buffer once it has played it, which paces whoever is writing. Commands go on
// their own queue, one at a time.

namespace Kernel {

namespace {

constexpr u16 device_id = 0x1059;

constexpr u16 control_queue = 0;
constexpr u16 transmit_queue = 2;

constexpr u32 config_offset_streams = 4;

constexpr u32 request_pcm_info = 0x0100;
constexpr u32 request_pcm_set_params = 0x0101;
constexpr u32 request_pcm_prepare = 0x0102;
constexpr u32 request_pcm_start = 0x0104;
constexpr u32 status_ok = 0x8000;

constexpr u8 direction_output = 0;
constexpr u8 format_s16 = 5;
constexpr u8 rate_48000 = 7;

constexpr u32 period_bytes = VULTURE_AUDIO_PERIOD_BYTES;
// How many periods the device holds at once: about 85 ms, little enough that a sound
// plays when it is asked for.
constexpr u32 slot_count = 4;
constexpr unsigned frames_order = 2;
static_assert(period_bytes * slot_count == page_size << frames_order);

constexpr u32 largest_stream_count = 16;

struct QueryInfo {
    u32 code;
    u32 start_id;
    u32 count;
    u32 size;
};
static_assert(sizeof(QueryInfo) == 16);

struct StreamInfo {
    u32 function_group;
    u32 features;
    u64 formats;
    u64 rates;
    u8 direction;
    u8 channels_min;
    u8 channels_max;
    u8 padding[5];
};
static_assert(sizeof(StreamInfo) == 32);

struct StreamCommand {
    u32 code;
    u32 stream;
};

struct SetParameters {
    u32 code;
    u32 stream;
    u32 buffer_bytes;
    u32 period_bytes;
    u32 features;
    u8 channels;
    u8 format;
    u8 rate;
    u8 padding;
};
static_assert(sizeof(SetParameters) == 24);

// One per slot, in a page: what the device reads and writes around the samples.
struct SlotHeader {
    u32 stream;
    u32 padding;
    u32 status;
    u32 latency_bytes;
};
static_assert(sizeof(SlotHeader) == 16);

class VirtIOSound final : public Device {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<VirtIOSound>> create(PCI::Device const& pci)
    {
        auto transport = TRY(try_make<VirtIO::Device>(pci));
        TRY(transport->initialize(0));
        auto control = TRY(transport->setup_queue(control_queue, 8));
        auto transmit = TRY(transport->setup_queue(transmit_queue, 16));
        auto control_memory = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
        auto header_memory = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
        auto frames = TRY(physical_memory().allocate_pages(frames_order, PageUse::General));
        auto* device = new (Base::nothrow) VirtIOSound(move(transport), move(control), move(transmit), control_memory, header_memory, frames);
        if (!device)
            return Error::from_errno(ENOMEM);
        auto reference = adopt_ref(*device);
        auto interrupt = pci.interrupt();
        if (!interrupt.has_value())
            return Error::from_errno(ENODEV);
        register_interrupt_handler(interrupt.value(), [](void* context) { static_cast<VirtIOSound*>(context)->handle_interrupt(); }, device);
        Arch::enable_interrupt(interrupt.value());
        device->m_transport->finish_initialization();
        auto thread = TRY(Thread::create("virtio-sound"_sv, [](void* context) { static_cast<VirtIOSound*>(context)->run(); }, device, { .qos = QoS::UserInteractive }));
        thread->start();
        TRY(device->configure());
        return reference;
    }

    ErrorOr<size_t> read(u64, size_t, IOBuffer const&) override { return Error::from_errno(EINVAL); }

    ErrorOr<size_t> write(u64, size_t count, IOBuffer const& buffer) override
    {
        if (count % VULTURE_AUDIO_BYTES_PER_FRAME != 0)
            return Error::from_errno(EINVAL);
        MutexLocker locker(m_write_lock);
        size_t done = 0;
        while (done < count) {
            if (!m_free_slots.load() && !m_room.wait_until_interruptible([&] { return m_free_slots.load() != 0; })) {
                if (done > 0)
                    return done;
                return Error::from_errno(EINTR);
            }
            u32 slot = static_cast<u32>(__builtin_ctz(m_free_slots.load()));
            size_t length = min<size_t>(count - done, period_bytes);
            auto* frames = reinterpret_cast<u8*>(physical_to_virtual(m_frames.offset(slot * period_bytes)));
            TRY(buffer.read(done, { frames, length }));
            auto* header = reinterpret_cast<SlotHeader*>(physical_to_virtual(m_headers.offset(slot * sizeof(SlotHeader))));
            header->stream = m_stream;
            VirtIO::Buffer const chain[3] = {
                { m_headers.offset(slot * sizeof(SlotHeader)), sizeof(u32), false },
                { m_frames.offset(slot * period_bytes), static_cast<u32>(length), false },
                { m_headers.offset(slot * sizeof(SlotHeader) + __builtin_offsetof(SlotHeader, status)), 2 * sizeof(u32), true },
            };
            auto head = m_transmit->submit({ chain, 3 });
            if (head.is_error())
                return head.release_error();
            m_free_slots.fetch_and(~(1u << slot));
            m_slot_of_head[head.value() % transmit_slots] = slot;
            m_transport->notify(*m_transmit);
            done += length;
            // The device does not start until it has something to play.
            if (!m_started) {
                m_started = true;
                StreamCommand command { request_pcm_start, m_stream };
                TRY(send_command(&command, sizeof(command)));
            }
        }
        return done;
    }

    [[nodiscard]] bool can_read() const override { return false; }
    [[nodiscard]] bool can_write() const override { return m_free_slots.load() != 0; }

private:
    // A descriptor chain per slot, of three, in a queue of this many.
    static constexpr u16 transmit_slots = 16;

    VirtIOSound(NonnullOwnPtr<VirtIO::Device> transport, NonnullOwnPtr<VirtIO::Queue> control, NonnullOwnPtr<VirtIO::Queue> transmit, PhysicalAddress control_memory, PhysicalAddress headers, PhysicalAddress frames)
        : Device(InodeType::CharacterDevice, 0600, device_number(14, 4))
        , m_transport(move(transport))
        , m_control(move(control))
        , m_transmit(move(transmit))
        , m_control_memory(control_memory)
        , m_headers(headers)
        , m_frames(frames)
    {
    }

    // The first output stream that plays what we write, set up to play it.
    ErrorOr<void> configure()
    {
        u32 streams = min(m_transport->config_read32(config_offset_streams), largest_stream_count);
        if (streams == 0)
            return Error::from_errno(ENODEV);
        QueryInfo query { request_pcm_info, 0, streams, sizeof(StreamInfo) };
        TRY(send_command(&query, sizeof(query), streams * sizeof(StreamInfo)));
        // Right after the status, so not aligned as a StreamInfo would be: copied out.
        auto const* answers = reinterpret_cast<u8 const*>(physical_to_virtual(m_control_memory.offset(response_offset + sizeof(u32))));
        Optional<u32> found;
        for (u32 i = 0; i < streams && !found.has_value(); ++i) {
            StreamInfo info;
            __builtin_memcpy(&info, answers + i * sizeof(StreamInfo), sizeof(info));
            if (info.direction == direction_output && (info.formats & (1ull << format_s16)) && (info.rates & (1ull << rate_48000))
                && info.channels_min <= VULTURE_AUDIO_CHANNELS && info.channels_max >= VULTURE_AUDIO_CHANNELS)
                found = i;
        }
        if (!found.has_value())
            return Error::from_errno(ENODEV);
        m_stream = found.value();

        SetParameters parameters {};
        parameters.code = request_pcm_set_params;
        parameters.stream = m_stream;
        parameters.buffer_bytes = period_bytes * slot_count;
        parameters.period_bytes = period_bytes;
        parameters.channels = VULTURE_AUDIO_CHANNELS;
        parameters.format = format_s16;
        parameters.rate = rate_48000;
        TRY(send_command(&parameters, sizeof(parameters)));
        StreamCommand prepare { request_pcm_prepare, m_stream };
        TRY(send_command(&prepare, sizeof(prepare)));
        dmesgln("Audio: virtio-sound, stream {} of {}: 48 kHz, stereo, 16-bit", m_stream, streams);
        return {};
    }

    // One at a time, and waits for the answer. The status comes first in what comes back;
    // `extra` bytes of answer follow it, where configure() reads them.
    ErrorOr<void> send_command(void const* request, size_t size, size_t extra = 0)
    {
        MutexLocker locker(m_control_lock);
        auto* page = reinterpret_cast<u8*>(physical_to_virtual(m_control_memory));
        __builtin_memcpy(page, request, size);
        auto* status = reinterpret_cast<u32*>(page + response_offset);
        *status = 0;
        VirtIO::Buffer const chain[2] = {
            { m_control_memory, static_cast<u32>(size), false },
            { m_control_memory.offset(response_offset), static_cast<u32>(sizeof(u32) + extra), true },
        };
        m_control_finished.store(false);
        TRY(m_control->submit({ chain, 2 }));
        m_transport->notify(*m_control);
        m_control_wait.wait_until([&] { return m_control_finished.load(); });
        if (*status != status_ok)
            return Error::from_errno(EIO);
        return {};
    }

    void handle_interrupt()
    {
        if (!(m_transport->read_interrupt_status() & 1))
            return;
        m_pending.store(true);
        m_wait.wake_all();
    }

    [[noreturn]] void run()
    {
        for (;;) {
            m_wait.wait_until([&] { return m_pending.load(); });
            m_pending.store(false);
            bool room = false;
            for (auto completion = m_transmit->take_completion(); completion.has_value(); completion = m_transmit->take_completion()) {
                m_free_slots.fetch_or(1u << m_slot_of_head[completion->id % transmit_slots]);
                room = true;
            }
            if (room) {
                m_room.wake_all();
                notify_pollers();
            }
            bool answered = false;
            for (auto completion = m_control->take_completion(); completion.has_value(); completion = m_control->take_completion())
                answered = true;
            if (answered) {
                m_control_finished.store(true);
                m_control_wait.wake_all();
            }
        }
    }

    // Requests are small; a page holds them and the answers side by side.
    static constexpr size_t response_offset = 512;

    NonnullOwnPtr<VirtIO::Device> m_transport;
    NonnullOwnPtr<VirtIO::Queue> m_control;
    NonnullOwnPtr<VirtIO::Queue> m_transmit;
    PhysicalAddress m_control_memory;
    PhysicalAddress m_headers;
    PhysicalAddress m_frames;
    u32 m_stream { 0 };
    bool m_started { false };
    Mutex m_write_lock;
    Mutex m_control_lock;
    Atomic<u32> m_free_slots { (1u << slot_count) - 1 };
    u32 m_slot_of_head[transmit_slots] {};
    Atomic<bool> m_pending { false };
    Atomic<bool> m_control_finished { false };
    WaitQueue m_wait;
    WaitQueue m_room;
    WaitQueue m_control_wait;
};

}

void initialize_audio()
{
    for (auto& pci : PCI::devices()) {
        if (pci.vendor_id() != VirtIO::vendor_id || pci.device_id() != device_id)
            continue;
        auto created = VirtIOSound::create(pci);
        if (created.is_error()) {
            dmesgln("Audio: virtio-sound at {:02x}:{:02x}.{}: {}", pci.bus(), pci.slot(), pci.function(), created.error());
            continue;
        }
        if (auto added = DevFS::the().add_device("audio0"_sv, NonnullRefPtr<Inode>(*static_cast<Inode*>(created.value().ptr()))); added.is_error())
            dmesgln("Audio: /dev/audio0: {}", added.error());
        return;
    }
}

}
