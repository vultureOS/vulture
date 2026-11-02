/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/MMIO.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Storage/StorageControllers.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Time/Time.h>
#include <LibBase/New.h>
#include <LibBase/String.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

// An SD card, through an SD Host Controller: how a Raspberry Pi 4 and 5 reach what they started from. (The
// registers are the SD Association's standard ones.) Data goes through the controller's data port a word at a
// time: there's no DMA here, whose address limits differ from one Pi to the next, and nothing to go wrong in
// a cache. The controller's interrupts aren't used either: this waits, usually for microseconds, sleeping when
// it's a long wait, as for a card finishing a write. It runs at the 25 MHz every card can do, on four wires.
//
// All accesses are 32 bits wide, as that's all some controllers do right; that's why the mode and the command
// go together.

namespace Kernel {

namespace {

using Arch::mmio_read32;
using Arch::mmio_write32;

constexpr FlatPtr block_size_and_count = 0x04;
constexpr FlatPtr argument_register = 0x08;
constexpr FlatPtr mode_and_command = 0x0c;
constexpr FlatPtr response = 0x10;
constexpr FlatPtr data_port = 0x20;
constexpr FlatPtr present_state = 0x24;
constexpr FlatPtr host_and_power = 0x28;
constexpr FlatPtr clock_timeout_and_reset = 0x2c;
constexpr FlatPtr interrupt_status = 0x30;
constexpr FlatPtr interrupt_status_enable = 0x34;
constexpr FlatPtr interrupt_signal_enable = 0x38;
constexpr FlatPtr capabilities = 0x40;
constexpr FlatPtr slot_and_version = 0xfc;

constexpr u32 state_command_inhibit = 1u << 0;
constexpr u32 state_data_inhibit = 1u << 1;

constexpr u32 host_four_wires = 1u << 1;
constexpr u32 power_on_at_3v3 = 0x0f00;

constexpr u32 clock_internal_enable = 1u << 0;
constexpr u32 clock_internal_stable = 1u << 1;
constexpr u32 clock_card_enable = 1u << 2;
constexpr u32 longest_data_timeout = 0x0e0000;
constexpr u32 reset_everything = 1u << 24;
constexpr u32 reset_command_line = 1u << 25;
constexpr u32 reset_data_lines = 1u << 26;

constexpr u32 status_command_complete = 1u << 0;
constexpr u32 status_transfer_complete = 1u << 1;
constexpr u32 status_write_ready = 1u << 4;
constexpr u32 status_read_ready = 1u << 5;
constexpr u32 status_any_error = 0xffff'8000;
// Everything used here: what's above, and every error.
constexpr u32 status_wanted = 0x01ff'0033;

constexpr u32 mode_block_count = 1u << 1;
constexpr u32 mode_stop_when_done = 1u << 2;
constexpr u32 mode_read = 1u << 4;
constexpr u32 mode_many_blocks = 1u << 5;

// The command half: what comes back, and how it's checked.
constexpr u32 reply_none = 0;
constexpr u32 reply_long = 1 | (1 << 3);
constexpr u32 reply_short_unchecked = 2;
constexpr u32 reply_short = 2 | (1 << 3) | (1 << 4);
constexpr u32 reply_short_then_busy = 3 | (1 << 3) | (1 << 4);
constexpr u32 command_has_data = 1u << 5;

constexpr u32 card_reports_ready = 1u << 31;
constexpr u32 card_is_high_capacity = 1u << 30;
constexpr u32 asking_for_high_capacity_at_3v3 = (1u << 30) | 0x00ff8000;
constexpr u32 voltage_check_pattern = 0x1aa;

constexpr size_t sector_size = BlockDevice::block_size;
constexpr size_t most_blocks_at_once = 2048;
constexpr u32 identification_clock_hz = 400'000;
constexpr u32 data_clock_hz = 25'000'000;

u64 s_clock_from_firmware = 0;

class SDCard final : public BlockDevice {
public:
    SDCard(String name, FlatPtr registers)
        : BlockDevice(move(name), 0, false)
        , m_registers(registers)
    {
    }

    ErrorOr<void> initialize()
    {
        put(clock_timeout_and_reset, reset_everything);
        if (!wait_for(100, [&] { return !(get(clock_timeout_and_reset) & reset_everything); }))
            return Error::from_string_literal("the controller won't reset");

        put(host_and_power, power_on_at_3v3);
        put(interrupt_status_enable, status_wanted);
        put(interrupt_signal_enable, 0);
        put(interrupt_status, ~0u);

        u32 const version = (get(slot_and_version) >> 16) & 0xff;
        m_divider_is_linear = version >= 2;
        m_base_clock = s_clock_from_firmware;
        if (m_base_clock == 0) {
            u32 const mask = m_divider_is_linear ? 0xff : 0x3f;
            m_base_clock = static_cast<u64>((get(capabilities) >> 8) & mask) * 1'000'000;
        }
        if (m_base_clock == 0)
            return Error::from_string_literal("nobody says what the controller's clock is");
        TRY(set_clock(identification_clock_hz));
        // A card wants a moment of clock before anything's said to it.
        Time::wait_until(Time::now() + Time::ticks_from_milliseconds(10));

        TRY(identify());
        TRY(set_clock(data_clock_hz));
        return {};
    }

    ErrorOr<void> read_blocks(u64 first, size_t count, Bytes buffer) override
    {
        if (first + count > block_count() || buffer.size() < count * sector_size)
            return Error::from_errno(EINVAL);
        MutexLocker locker(m_lock);
        size_t done = 0;
        while (done < count) {
            size_t const blocks = min(count - done, most_blocks_at_once);
            TRY(transfer(first + done, blocks, buffer.data() + done * sector_size, false));
            done += blocks;
        }
        return {};
    }

    ErrorOr<void> write_blocks(u64 first, size_t count, ReadonlyBytes buffer) override
    {
        if (first + count > block_count() || buffer.size() < count * sector_size)
            return Error::from_errno(EINVAL);
        MutexLocker locker(m_lock);
        size_t done = 0;
        while (done < count) {
            size_t const blocks = min(count - done, most_blocks_at_once);
            TRY(transfer(first + done, blocks, const_cast<u8*>(buffer.data()) + done * sector_size, true));
            done += blocks;
        }
        return {};
    }

    // When a write's finished, it's on the card: that's what the wait before the next command is for.
    ErrorOr<void> flush() override
    {
        MutexLocker locker(m_lock);
        if (!wait_for(5000, [&] { return !(get(present_state) & state_data_inhibit); }))
            return Error::from_errno(EIO);
        return {};
    }

    [[nodiscard]] u64 capacity_in_bytes() const { return block_count() * sector_size; }

private:
    [[nodiscard]] u32 get(FlatPtr offset) const { return mmio_read32(m_registers + offset); }
    void put(FlatPtr offset, u32 value) { mmio_write32(m_registers + offset, value); }

    template<typename Condition>
    bool wait_for(u64 milliseconds, Condition condition)
    {
        constexpr u32 spins_before_sleeping = 5000;
        u64 const deadline = Time::now() + Time::ticks_from_milliseconds(milliseconds);
        u32 spins = 0;
        while (!condition()) {
            if (Time::now() > deadline)
                return false;
            if (++spins < spins_before_sleeping)
                Arch::pause();
            else
                Time::wait_until(Time::now() + Time::ticks_from_milliseconds(1));
        }
        return true;
    }

    ErrorOr<void> set_clock(u32 hz)
    {
        put(clock_timeout_and_reset, longest_data_timeout);
        u32 divider = 0;
        if (m_divider_is_linear) {
            // Clock = base / (2 × divider), or base itself if that's 0.
            if (m_base_clock > hz)
                divider = min<u64>((m_base_clock + 2 * hz - 1) / (2 * hz), 1023);
        } else {
            // A power of 2, up to 256.
            divider = 1;
            while (divider < 128 && m_base_clock / (2 * divider) > hz)
                divider *= 2;
        }
        u32 const field = ((divider & 0xff) << 8) | (((divider >> 8) & 3) << 6);
        put(clock_timeout_and_reset, longest_data_timeout | field | clock_internal_enable);
        if (!wait_for(100, [&] { return get(clock_timeout_and_reset) & clock_internal_stable; }))
            return Error::from_string_literal("the controller's clock won't settle");
        put(clock_timeout_and_reset, longest_data_timeout | field | clock_internal_enable | clock_card_enable);
        return {};
    }

    void recover(bool with_data)
    {
        u32 const lines = reset_command_line | (with_data ? reset_data_lines : 0);
        u32 const clock = get(clock_timeout_and_reset) & 0x00ff'ffff;
        put(clock_timeout_and_reset, clock | lines);
        (void)wait_for(100, [&] { return !(get(clock_timeout_and_reset) & lines); });
        put(interrupt_status, ~0u);
    }

    // Waits for these to be set, and clears them; a reported error, or nothing happening, is a failure.
    ErrorOr<void> wait_for_status(u32 bits, u64 milliseconds)
    {
        u32 status = 0;
        bool const seen = wait_for(milliseconds, [&] {
            status = get(interrupt_status);
            return (status & (bits | status_any_error)) != 0;
        });
        if (!seen)
            return Error::from_errno(ETIMEDOUT);
        if (status & status_any_error)
            return Error::from_errno(EIO);
        put(interrupt_status, status & bits);
        return {};
    }

    ErrorOr<void> command(u32 index, u32 argument, u32 reply)
    {
        if (!wait_for(1000, [&] { return !(get(present_state) & state_command_inhibit); }))
            return Error::from_errno(ETIMEDOUT);
        // One that waits on the card being busy has to wait for the data lines too.
        if (reply == reply_short_then_busy && !wait_for(1000, [&] { return !(get(present_state) & state_data_inhibit); }))
            return Error::from_errno(ETIMEDOUT);
        put(interrupt_status, ~0u);
        put(argument_register, argument);
        put(mode_and_command, ((index << 8) | reply) << 16);
        auto result = wait_for_status(status_command_complete, 1000);
        if (result.is_error()) {
            recover(false);
            return result;
        }
        if (reply == reply_short_then_busy) {
            if (!wait_for(1000, [&] { return !(get(present_state) & state_data_inhibit); }))
                return Error::from_errno(ETIMEDOUT);
            put(interrupt_status, status_transfer_complete);
        }
        return {};
    }

    ErrorOr<void> application_command(u32 index, u32 argument, u32 reply)
    {
        TRY(command(55, m_address << 16, reply_short));
        return command(index, argument, reply);
    }

    // From the long reply: bits of what the card said, less the checksum at the bottom, which the controller keeps.
    [[nodiscard]] u32 long_reply_bits(u32 const (&words)[4], u32 lowest, u32 count) const
    {
        u32 value = 0;
        for (u32 i = 0; i < count; ++i) {
            u32 const bit = lowest + i;
            value |= ((words[bit / 32] >> (bit % 32)) & 1) << i;
        }
        return value;
    }

    ErrorOr<void> identify()
    {
        TRY(command(0, 0, reply_none));
        // Cards older than version 2 don't answer this one; they don't have to.
        bool const answers_voltage_check = !command(8, voltage_check_pattern, reply_short).is_error();
        if (answers_voltage_check && (get(response) & 0xfff) != voltage_check_pattern)
            return Error::from_string_literal("the card doesn't take this voltage");

        u32 const offer = answers_voltage_check ? asking_for_high_capacity_at_3v3 : (asking_for_high_capacity_at_3v3 & ~card_is_high_capacity);
        u64 const deadline = Time::now() + Time::ticks_from_milliseconds(2000);
        u32 reply = 0;
        do {
            TRY(application_command(41, offer, reply_short_unchecked));
            reply = get(response);
            if (reply & card_reports_ready)
                break;
            Time::wait_until(Time::now() + Time::ticks_from_milliseconds(10));
        } while (Time::now() < deadline);
        if (!(reply & card_reports_ready))
            return Error::from_string_literal("the card never says it's ready");
        m_high_capacity = reply & card_is_high_capacity;

        TRY(command(2, 0, reply_long));
        TRY(command(3, 0, reply_short));
        m_address = get(response) >> 16;

        // The size, from its "specific data". Bit numbers here are the specification's, less the 8 of the checksum.
        TRY(command(9, m_address << 16, reply_long));
        u32 const words[4] { get(response), get(response + 4), get(response + 8), get(response + 12) };
        u64 blocks = 0;
        if (long_reply_bits(words, 118, 2) == 1) {
            blocks = (static_cast<u64>(long_reply_bits(words, 40, 22)) + 1) * 1024;
        } else {
            u32 const read_block_length = long_reply_bits(words, 72, 4);
            u32 const size = long_reply_bits(words, 54, 12);
            u32 const multiplier = long_reply_bits(words, 39, 3);
            blocks = (static_cast<u64>(size + 1) << (multiplier + 2)) << read_block_length;
            blocks /= sector_size;
        }
        if (blocks == 0)
            return Error::from_string_literal("the card doesn't say how big it is");
        set_block_count(blocks);

        TRY(command(7, m_address << 16, reply_short_then_busy));
        if (!m_high_capacity)
            TRY(command(16, sector_size, reply_short));
        // Four wires instead of one.
        TRY(application_command(6, 2, reply_short));
        put(host_and_power, get(host_and_power) | host_four_wires);
        return {};
    }

    ErrorOr<void> transfer(u64 first, size_t blocks, u8* data, bool is_write)
    {
        // A card still writing what came before says so on its data lines.
        if (!wait_for(5000, [&] { return !(get(present_state) & (state_command_inhibit | state_data_inhibit)); })) {
            recover(true);
            return Error::from_errno(ETIMEDOUT);
        }
        put(interrupt_status, ~0u);
        put(block_size_and_count, static_cast<u32>(sector_size) | (static_cast<u32>(blocks) << 16));
        put(argument_register, static_cast<u32>(m_high_capacity ? first : first * sector_size));

        u32 mode = is_write ? 0 : mode_read;
        u32 index = is_write ? 24 : 17;
        if (blocks > 1) {
            mode |= mode_block_count | mode_many_blocks | mode_stop_when_done;
            index = is_write ? 25 : 18;
        }
        put(mode_and_command, mode | (((index << 8) | reply_short | command_has_data) << 16));

        auto result = move_data(blocks, data, is_write);
        if (result.is_error())
            recover(true);
        return result;
    }

    ErrorOr<void> move_data(size_t blocks, u8* data, bool is_write)
    {
        TRY(wait_for_status(status_command_complete, 1000));
        for (size_t block = 0; block < blocks; ++block) {
            TRY(wait_for_status(is_write ? status_write_ready : status_read_ready, 5000));
            for (size_t offset = 0; offset < sector_size; offset += 4) {
                u32 word;
                if (is_write) {
                    __builtin_memcpy(&word, data + offset, 4);
                    put(data_port, word);
                } else {
                    word = get(data_port);
                    __builtin_memcpy(data + offset, &word, 4);
                }
            }
            data += sector_size;
        }
        TRY(wait_for_status(status_transfer_complete, 5000));
        return {};
    }

    FlatPtr m_registers { 0 };
    u64 m_base_clock { 0 };
    u32 m_address { 0 };
    bool m_divider_is_linear { true };
    bool m_high_capacity { false };
    Mutex m_lock;
};

}

void set_sd_controller_clock(u64 hz)
{
    s_clock_from_firmware = hz;
}

void initialize_sd_cards()
{
    auto tree = DeviceTree::FlattenedDeviceTree::parse(memory_manager().device_tree());
    if (tree.is_error())
        return;
    // The Pi 4's and the Pi 5's. (The Pi has other SD controllers, for Wi-Fi: not these.)
    Optional<DeviceTree::Node> node;
    static constexpr StringView controllers[] = { "brcm,bcm2711-emmc2"_sv, "brcm,bcm2712-sdhci"_sv };
    for (auto compatible : controllers) {
        node = tree.value().find_compatible_node(compatible);
        if (node.has_value())
            break;
    }
    if (!node.has_value())
        return;
    auto region = tree.value().reg(node.value(), 0);
    if (!region.has_value())
        return;
    auto registers = memory_manager().map_mmio(PhysicalAddress(region->address), max<u64>(region->size, 0x100));
    if (registers.is_error()) {
        dmesgln("Storage: SD controller at {:#x}: {}", region->address, registers.error());
        return;
    }
    StringBuilder name;
    name.appendff("disk{}", take_disk_number());
    auto* card = new (Base::nothrow) SDCard(MUST(name.to_string()), registers.value());
    if (!card)
        return;
    auto device = adopt_ref(*card);
    if (auto result = card->initialize(); result.is_error()) {
        dmesgln("Storage: no SD card: {}", result.error());
        return;
    }
    StringBuilder description;
    description.appendff("SD card, {} MiB", card->capacity_in_bytes() / MiB);
    add_disk(device, description.view());
}

}
