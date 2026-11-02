/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/audio.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/MMIO.h>
#include <Kernel/Bus/PCI/PCI.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/FileSystem/DevFS.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Time/Time.h>
#include <LibBase/Atomic.h>
#include <LibBase/New.h>

// Intel High Definition Audio: the sound of practically every PC. The controller runs streams of samples
// from memory, and talks to the codecs (the chips that make them into sound) by sending them verbs.
// A codec is a tree of widgets, some of which are converters and some the jacks and speakers, and it
// is up to this to find a way from one to the other and turn everything on along it.
//
// /dev/audio0 works as it does with virtio-sound: see <Kernel/API/POSIX/audio.h>.

namespace Kernel {

void initialize_intel_hda();

namespace {

using Arch::mmio_read16;
using Arch::mmio_read32;
using Arch::mmio_read8;
using Arch::mmio_write16;
using Arch::mmio_write32;
using Arch::mmio_write8;

constexpr FlatPtr reg_capabilities = 0x00;
constexpr FlatPtr reg_control = 0x08;
constexpr FlatPtr reg_codec_status = 0x0e;
constexpr FlatPtr reg_interrupt_control = 0x20;
constexpr FlatPtr reg_corb_low = 0x40;
constexpr FlatPtr reg_corb_high = 0x44;
constexpr FlatPtr reg_corb_write = 0x48;
constexpr FlatPtr reg_corb_read = 0x4a;
constexpr FlatPtr reg_corb_control = 0x4c;
constexpr FlatPtr reg_corb_size = 0x4e;
constexpr FlatPtr reg_rirb_low = 0x50;
constexpr FlatPtr reg_rirb_high = 0x54;
constexpr FlatPtr reg_rirb_write = 0x58;
constexpr FlatPtr reg_rirb_count = 0x5a;
constexpr FlatPtr reg_rirb_control = 0x5c;
constexpr FlatPtr reg_rirb_status = 0x5d;
constexpr FlatPtr reg_rirb_size = 0x5e;
constexpr FlatPtr reg_first_stream = 0x80;
constexpr FlatPtr stream_register_size = 0x20;

constexpr FlatPtr stream_control = 0x00;
constexpr FlatPtr stream_status = 0x03;
constexpr FlatPtr stream_length = 0x08;
constexpr FlatPtr stream_last_index = 0x0c;
constexpr FlatPtr stream_format = 0x12;
constexpr FlatPtr stream_list_low = 0x18;
constexpr FlatPtr stream_list_high = 0x1c;

constexpr u8 stream_reset = 1 << 0;
constexpr u8 stream_run = 1 << 1;
constexpr u8 stream_interrupt_on_completion = 1 << 2;
constexpr u8 stream_status_completion = 1 << 2;

// Verbs.
constexpr u32 verb_get_parameter = 0xf00;
constexpr u32 verb_get_connection_list = 0xf02;
constexpr u32 verb_set_connection_select = 0x701;
constexpr u32 verb_set_power_state = 0x705;
constexpr u32 verb_set_stream = 0x706;
constexpr u32 verb_set_pin_control = 0x707;
constexpr u32 verb_set_eapd = 0x70c;
constexpr u32 verb_get_configuration = 0xf1c;
constexpr u32 verb_set_format = 0x200;
constexpr u32 verb_set_amplifier = 0x300;

constexpr u32 parameter_node_count = 0x04;
constexpr u32 parameter_function_type = 0x05;
constexpr u32 parameter_widget_capabilities = 0x09;
constexpr u32 parameter_pin_capabilities = 0x0c;
constexpr u32 parameter_output_amplifier = 0x12;
constexpr u32 parameter_connection_length = 0x0e;

constexpr u32 widget_output = 0;
constexpr u32 widget_mixer = 2;
constexpr u32 widget_selector = 3;
constexpr u32 widget_pin = 4;

// 48 kHz, 16 bits, two channels: as the stream's format register, and a converter's.
constexpr u16 format_48k_16_stereo = 0x0011;
constexpr u8 stream_tag = 1;

constexpr size_t period_bytes = VULTURE_AUDIO_PERIOD_BYTES;
constexpr size_t period_count = 4;
constexpr size_t ring_bytes = period_bytes * period_count;
static_assert(ring_bytes <= page_size * 8);

struct BufferEntry {
    u64 address;
    u32 length;
    u32 flags; // 1: interrupt when this is done.
};
static_assert(sizeof(BufferEntry) == 16);

class IntelHDA final : public Device {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<IntelHDA>> create(PCI::Device const& pci)
    {
        auto const bar = pci.bar(0);
        if (!bar.is_memory || bar.address == 0)
            return Error::from_string_literal("no registers");
        pci.enable_memory_and_bus_mastering();
        FlatPtr const registers = TRY(memory_manager().map_mmio(PhysicalAddress(bar.address), bar.size ? bar.size : 16 * KiB));

        auto* device = new (Base::nothrow) IntelHDA(registers);
        if (!device)
            return Error::from_errno(ENOMEM);
        auto reference = adopt_ref(*device);
        TRY(device->reset_controller());
        TRY(device->start_command_rings());
        TRY(device->find_output());
        TRY(device->prepare_stream());

        auto const interrupt = pci.interrupt();
        if (!interrupt.has_value())
            return Error::from_errno(ENODEV);
        register_interrupt_handler(interrupt.value(), [](void* context) { static_cast<IntelHDA*>(context)->handle_interrupt(); }, device);
        Arch::enable_interrupt(interrupt.value());
        // On, overall, and for the one stream.
        mmio_write32(registers + reg_interrupt_control, (1u << 31) | (1u << device->m_stream_index));
        return reference;
    }

    ErrorOr<size_t> read(u64, size_t, IOBuffer const&) override { return Error::from_errno(EINVAL); }

    ErrorOr<size_t> write(u64, size_t count, IOBuffer const& buffer) override
    {
        if (count % VULTURE_AUDIO_BYTES_PER_FRAME != 0)
            return Error::from_errno(EINVAL);
        MutexLocker locker(m_write_lock);
        size_t done = 0;
        auto* ring = physical_to_virtual(m_samples);
        while (done < count) {
            if (room() == 0 && !m_room.wait_until_interruptible([this] { return room() != 0; })) {
                if (done > 0)
                    return done;
                return Error::from_errno(EINTR);
            }
            u64 const written = m_written.load();
            size_t const offset = written % ring_bytes;
            // Up to the end of what can be written, and of the ring.
            size_t const length = min(min(count - done, room()), ring_bytes - offset);
            TRY(buffer.read(done, { ring + offset, length }));
            Arch::device_barrier();
            m_written.store(written + length);
            done += length;
            start_if_stopped();
        }
        return done;
    }

    [[nodiscard]] bool can_read() const override { return false; }
    [[nodiscard]] bool can_write() const override { return room() != 0; }

private:
    explicit IntelHDA(FlatPtr registers)
        : Device(InodeType::CharacterDevice, 0660, device_number(14, 1))
        , m_registers(registers)
    {
    }

    // How much more can be taken before it would overwrite what's still to be played.
    [[nodiscard]] size_t room() const
    {
        return ring_bytes - static_cast<size_t>(m_written.load() - m_played.load());
    }

    [[nodiscard]] FlatPtr stream_register(FlatPtr offset) const { return m_stream_registers + offset; }

    ErrorOr<void> reset_controller()
    {
        mmio_write32(m_registers + reg_control, mmio_read32(m_registers + reg_control) & ~1u);
        TRY(wait_for([this] { return !(mmio_read32(m_registers + reg_control) & 1); }, "enter reset"));
        mmio_write32(m_registers + reg_control, mmio_read32(m_registers + reg_control) | 1);
        TRY(wait_for([this] { return mmio_read32(m_registers + reg_control) & 1; }, "leave reset"));
        // Codecs have this long to say they're there (the specification's figure).
        Time::wait_until(Time::now() + Time::ticks_from_microseconds(1000));
        u16 const capabilities = mmio_read16(m_registers + reg_capabilities);
        u32 const input_streams = (capabilities >> 8) & 0xf;
        u32 const output_streams = (capabilities >> 12) & 0xf;
        if (output_streams == 0)
            return Error::from_string_literal("no output streams");
        // Input streams come first, and the bit in the interrupt control register counts them too.
        m_stream_index = input_streams;
        m_stream_registers = m_registers + reg_first_stream + input_streams * stream_register_size;
        return {};
    }

    template<typename Condition>
    ErrorOr<void> wait_for(Condition condition, char const*)
    {
        u64 const deadline = Time::now() + Time::ticks_from_milliseconds(100);
        while (!condition()) {
            if (Time::now() > deadline)
                return Error::from_string_literal("the controller isn't responding");
            Arch::pause();
        }
        return {};
    }

    ErrorOr<void> start_command_rings()
    {
        // A page: the 256 commands going out, then the 256 answers.
        m_commands = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
        PhysicalAddress const answers = m_commands.offset(1024);

        mmio_write8(m_registers + reg_corb_control, 0);
        mmio_write8(m_registers + reg_rirb_control, 0);
        TRY(wait_for([this] { return !(mmio_read8(m_registers + reg_corb_control) & 2) && !(mmio_read8(m_registers + reg_rirb_control) & 2); }, "stop"));

        mmio_write32(m_registers + reg_corb_low, static_cast<u32>(m_commands.get()));
        mmio_write32(m_registers + reg_corb_high, static_cast<u32>(m_commands.get() >> 32));
        mmio_write8(m_registers + reg_corb_size, 2); // 256 entries.
        // The read pointer's reset takes setting, seeing it set, clearing, seeing it clear.
        mmio_write16(m_registers + reg_corb_read, 1 << 15);
        (void)wait_for([this] { return mmio_read16(m_registers + reg_corb_read) & (1 << 15); }, "reset");
        mmio_write16(m_registers + reg_corb_read, 0);
        (void)wait_for([this] { return !(mmio_read16(m_registers + reg_corb_read) & (1 << 15)); }, "reset");
        mmio_write16(m_registers + reg_corb_write, 0);

        mmio_write32(m_registers + reg_rirb_low, static_cast<u32>(answers.get()));
        mmio_write32(m_registers + reg_rirb_high, static_cast<u32>(answers.get() >> 32));
        mmio_write8(m_registers + reg_rirb_size, 2);
        mmio_write16(m_registers + reg_rirb_write, 1 << 15);
        mmio_write16(m_registers + reg_rirb_count, 1);
        mmio_write8(m_registers + reg_corb_control, 2);
        // Running, and setting the status bit for each answer, which send() clears: this polls, as nothing
        // lets commands' interrupts out of the controller, but it may wait for that bit.
        mmio_write8(m_registers + reg_rirb_control, 3);
        m_answer_read = 0;
        m_command_write = 0;
        return {};
    }

    // A verb, and its answer.
    Optional<u32> send(u32 codec, u32 node, u32 verb, u32 payload)
    {
        // Twelve-bit verbs have an 8-bit payload. The rest have four bits, and 16: those are given here
        // as a verb that ends in zeros.
        u32 const command = (codec << 28) | (node << 20) | (verb << 8) | payload;
        auto* commands = reinterpret_cast<u32*>(physical_to_virtual(m_commands));
        auto const* answers = reinterpret_cast<u64 const volatile*>(physical_to_virtual(m_commands.offset(1024)));
        m_command_write = static_cast<u16>((m_command_write + 1) % 256);
        commands[m_command_write] = command;
        Arch::device_barrier();
        mmio_write16(m_registers + reg_corb_write, m_command_write);
        u64 const deadline = Time::now() + Time::ticks_from_milliseconds(100);
        for (;;) {
            u16 const written = mmio_read16(m_registers + reg_rirb_write) & 0xff;
            if (written != m_answer_read)
                break;
            if (Time::now() > deadline)
                return {};
            Arch::pause();
        }
        m_answer_read = static_cast<u16>((m_answer_read + 1) % 256);
        Arch::memory_barrier();
        u64 const answer = answers[m_answer_read];
        mmio_write8(m_registers + reg_rirb_status, 5);
        return static_cast<u32>(answer);
    }

    Optional<u32> parameter(u32 codec, u32 node, u32 which)
    {
        return send(codec, node, verb_get_parameter, which);
    }

    // The nodes a widget can take its input from.
    size_t connections_of(u32 codec, u32 node, u16 (&list)[16])
    {
        auto const length_word = parameter(codec, node, parameter_connection_length);
        if (!length_word.has_value())
            return 0;
        bool const long_form = length_word.value() & (1 << 7);
        size_t const length = length_word.value() & 0x7f;
        size_t count = 0;
        size_t const per_answer = long_form ? 2 : 4;
        for (size_t index = 0; index < length && count < 16; index += per_answer) {
            auto const answer = send(codec, node, verb_get_connection_list, static_cast<u32>(index));
            if (!answer.has_value())
                break;
            for (size_t i = 0; i < per_answer && index + i < length && count < 16; ++i) {
                u32 const entry = long_form ? (answer.value() >> (16 * i)) & 0xffff : (answer.value() >> (8 * i)) & 0xff;
                u32 const mask = long_form ? 0x7fff : 0x7f;
                bool const is_range = entry & (long_form ? 0x8000 : 0x80);
                u16 const id = static_cast<u16>(entry & mask);
                if (is_range && count > 0) {
                    // From the last one to this one.
                    for (u16 next = static_cast<u16>(list[count - 1] + 1); next <= id && count < 16; ++next)
                        list[count++] = next;
                } else {
                    list[count++] = id;
                }
            }
        }
        return count;
    }

    // Finds a way from `node` back to an output converter, turning on what's on the way as it goes back
    // up, if it finds one.
    bool route_to_converter(u32 codec, u32 node, int depth)
    {
        if (depth > 8)
            return false;
        auto const capabilities = parameter(codec, node, parameter_widget_capabilities);
        if (!capabilities.has_value())
            return false;
        u32 const type = (capabilities.value() >> 20) & 0xf;
        if (type == widget_output) {
            (void)send(codec, node, verb_set_power_state, 0);
            (void)send(codec, node, verb_set_stream, (stream_tag << 4) | 0);
            (void)send(codec, node, verb_set_format, format_48k_16_stereo);
            unmute(codec, node);
            return true;
        }
        if (type != widget_mixer && type != widget_selector && type != widget_pin)
            return false;
        u16 list[16];
        size_t const count = connections_of(codec, node, list);
        for (size_t i = 0; i < count; ++i) {
            if (!route_to_converter(codec, list[i], depth + 1))
                continue;
            // A mixer takes all of its inputs; only others choose.
            if (type != widget_mixer && count > 1)
                (void)send(codec, node, verb_set_connection_select, static_cast<u32>(i));
            (void)send(codec, node, verb_set_power_state, 0);
            unmute(codec, node);
            // And its input side too, for what mixes: that's the amplifier at this index.
            if (type == widget_mixer)
                (void)send(codec, node, verb_set_amplifier, 0x7000 | static_cast<u32>(i << 8) | 0x7f);
            return true;
        }
        return false;
    }

    void unmute(u32 codec, u32 node)
    {
        // The output side, both channels, as loud as it says it goes. Its steps are in the top of this.
        u32 steps = 0x7f;
        if (auto const amplifier = parameter(codec, node, parameter_output_amplifier); amplifier.has_value() && amplifier.value() != 0)
            steps = (amplifier.value() >> 8) & 0x7f;
        (void)send(codec, node, verb_set_amplifier, 0xb000 | steps);
    }

    ErrorOr<void> find_output()
    {
        u16 const codecs = mmio_read16(m_registers + reg_codec_status);
        bool found = false;
        for (u32 codec = 0; codec < 15; ++codec) {
            if (!(codecs & (1u << codec)))
                continue;
            auto const root = parameter(codec, 0, parameter_node_count);
            if (!root.has_value())
                continue;
            u32 const first_group = (root.value() >> 16) & 0xff;
            u32 const groups = root.value() & 0xff;
            for (u32 group = first_group; group < first_group + groups; ++group) {
                auto const type = parameter(codec, group, parameter_function_type);
                if (!type.has_value() || (type.value() & 0xff) != 1)
                    continue;
                (void)send(codec, group, verb_set_power_state, 0);
                auto const widgets = parameter(codec, group, parameter_node_count);
                if (!widgets.has_value())
                    continue;
                u32 const first_widget = (widgets.value() >> 16) & 0xff;
                u32 const widget_count = widgets.value() & 0xff;
                for (u32 node = first_widget; node < first_widget + widget_count; ++node) {
                    auto const capabilities = parameter(codec, node, parameter_widget_capabilities);
                    if (!capabilities.has_value() || ((capabilities.value() >> 20) & 0xf) != widget_pin)
                        continue;
                    auto const pin = parameter(codec, node, parameter_pin_capabilities);
                    if (!pin.has_value() || !(pin.value() & (1 << 4)))
                        continue;
                    auto const configuration = send(codec, node, verb_get_configuration, 0);
                    if (!configuration.has_value())
                        continue;
                    // What the machine's maker says it's connected to. 01 means nothing is.
                    if ((configuration.value() >> 30) == 1)
                        continue;
                    u32 const device = (configuration.value() >> 20) & 0xf;
                    constexpr u32 device_line_out = 0;
                    constexpr u32 device_speaker = 1;
                    constexpr u32 device_headphones = 2;
                    if (device != device_line_out && device != device_speaker && device != device_headphones)
                        continue;
                    if (!route_to_converter(codec, node, 0))
                        continue;
                    constexpr u32 pin_output = 1 << 6;
                    constexpr u32 pin_headphones = 1 << 7;
                    (void)send(codec, node, verb_set_pin_control, pin_output | (device == device_headphones ? pin_headphones : 0));
                    // The amplifier some have, outside of the codec.
                    (void)send(codec, node, verb_set_eapd, 2);
                    found = true;
                    dmesgln("Audio: Intel HDA: codec {}, {} at node {}", codec, device == device_speaker ? "speaker"_sv : (device == device_headphones ? "headphones"_sv : "line out"_sv), node);
                }
            }
        }
        if (!found)
            return Error::from_string_literal("no codec has an output with a way to it");
        return {};
    }

    ErrorOr<void> prepare_stream()
    {
        m_samples = TRY(physical_memory().allocate_pages(3, PageUse::General));
        __builtin_memset(physical_to_virtual(m_samples), 0, ring_bytes);
        m_buffer_list = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
        auto* list = reinterpret_cast<BufferEntry*>(physical_to_virtual(m_buffer_list));
        for (size_t i = 0; i < period_count; ++i) {
            list[i].address = m_samples.get() + i * period_bytes;
            list[i].length = period_bytes;
            list[i].flags = 1;
        }

        // Stopped, reset, and with its number in the top byte of the control register.
        mmio_write8(stream_register(stream_control), 0);
        mmio_write8(stream_register(stream_control), stream_reset);
        TRY(wait_for([this] { return mmio_read8(stream_register(stream_control)) & stream_reset; }, "reset"));
        mmio_write8(stream_register(stream_control), 0);
        TRY(wait_for([this] { return !(mmio_read8(stream_register(stream_control)) & stream_reset); }, "reset"));
        mmio_write8(stream_register(stream_control) + 2, stream_tag << 4);
        mmio_write32(stream_register(stream_length), ring_bytes);
        mmio_write16(stream_register(stream_last_index), period_count - 1);
        mmio_write16(stream_register(stream_format), format_48k_16_stereo);
        mmio_write32(stream_register(stream_list_low), static_cast<u32>(m_buffer_list.get()));
        mmio_write32(stream_register(stream_list_high), static_cast<u32>(m_buffer_list.get() >> 32));
        return {};
    }

    void start_if_stopped()
    {
        SpinLockGuard guard(m_stream_lock);
        if (m_running)
            return;
        m_running = true;
        // (It starts at the top of the ring, which is where what's written is: see where it stops.)
        m_next_period = 0;
        mmio_write8(stream_register(stream_status), 0x1c);
        mmio_write8(stream_register(stream_control), stream_run | stream_interrupt_on_completion);
    }

    // Each time it gets to the end of a period: that much has been played, and is silence from now on
    // for when it comes round again, unless something has been written there by then.
    void handle_interrupt()
    {
        u8 const status = mmio_read8(stream_register(stream_status));
        if (!(status & stream_status_completion))
            return;
        mmio_write8(stream_register(stream_status), status);
        SpinLockGuard guard(m_stream_lock);
        if (!m_running)
            return;
        auto* ring = physical_to_virtual(m_samples);
        __builtin_memset(ring + m_next_period * period_bytes, 0, period_bytes);
        m_next_period = (m_next_period + 1) % period_count;
        u64 const played = m_played.load() + period_bytes;
        if (played >= m_written.load()) {
            // Nothing more to play.
            m_played.store(m_written.load());
            m_running = false;
            mmio_write8(stream_register(stream_control), stream_interrupt_on_completion);
            // It goes back to the start of the ring when it next runs, so this does too.
            mmio_write8(stream_register(stream_control), stream_reset);
            for (int i = 0; i < 1000 && !(mmio_read8(stream_register(stream_control)) & stream_reset); ++i)
                Arch::pause();
            mmio_write8(stream_register(stream_control), 0);
            for (int i = 0; i < 1000 && (mmio_read8(stream_register(stream_control)) & stream_reset); ++i)
                Arch::pause();
            mmio_write8(stream_register(stream_control) + 2, stream_tag << 4);
            mmio_write32(stream_register(stream_length), ring_bytes);
            mmio_write16(stream_register(stream_last_index), period_count - 1);
            mmio_write16(stream_register(stream_format), format_48k_16_stereo);
            mmio_write32(stream_register(stream_list_low), static_cast<u32>(m_buffer_list.get()));
            mmio_write32(stream_register(stream_list_high), static_cast<u32>(m_buffer_list.get() >> 32));
            u64 const aligned = m_written.load() + (ring_bytes - m_written.load() % ring_bytes) % ring_bytes;
            m_written.store(aligned);
            m_played.store(aligned);
        } else {
            m_played.store(played);
        }
        m_room.wake_all();
        notify_pollers();
    }

    FlatPtr m_registers;
    FlatPtr m_stream_registers { 0 };
    u32 m_stream_index { 0 };
    PhysicalAddress m_commands;
    PhysicalAddress m_samples;
    PhysicalAddress m_buffer_list;
    u16 m_command_write { 0 };
    u16 m_answer_read { 0 };
    Mutex m_write_lock;
    SpinLock m_stream_lock;
    bool m_running { false };
    size_t m_next_period { 0 };
    Atomic<u64> m_written { 0 };
    Atomic<u64> m_played { 0 };
    WaitQueue m_room;
};

}

void initialize_intel_hda()
{
    for (auto& pci : PCI::devices()) {
        // A multimedia device; an audio one, of this kind.
        if (pci.class_code() != 0x04 || pci.subclass() != 0x03)
            continue;
        auto created = IntelHDA::create(pci);
        if (created.is_error()) {
            dmesgln("Audio: Intel HDA at {:02x}:{:02x}.{}: {}", pci.bus(), pci.slot(), pci.function(), created.error());
            continue;
        }
        if (auto added = DevFS::the().add_device("audio0"_sv, NonnullRefPtr<Inode>(*static_cast<Inode*>(created.value().ptr()))); added.is_error())
            dmesgln("Audio: /dev/audio0: {}", added.error());
        return;
    }
}

}
