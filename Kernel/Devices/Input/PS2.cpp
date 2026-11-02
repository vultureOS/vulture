/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/input.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/x86_64/APIC.h>
#include <Kernel/Arch/x86_64/MMIO.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Input/Input.h>
#include <Kernel/Firmware/ACPI.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Tasks/Thread.h>
#include <Kernel/Time/Time.h>
#include <LibBase/Atomic.h>

// The keyboard and mouse ports of a PC, through the 8042 controller: still how the keyboard of many
// laptops is connected. The interrupts put what arrives in a ring, and a thread makes events of it.

namespace Kernel {

void initialize_ps2_input();

namespace {

constexpr u16 port_data = 0x60;
constexpr u16 port_status = 0x64;
constexpr u16 port_command = 0x64;

constexpr u8 status_output_full = 1 << 0;
constexpr u8 status_input_full = 1 << 1;
constexpr u8 status_from_mouse = 1 << 5;

constexpr u8 command_read_config = 0x20;
constexpr u8 command_write_config = 0x60;
constexpr u8 command_disable_mouse = 0xa7;
constexpr u8 command_enable_mouse = 0xa8;
constexpr u8 command_self_test = 0xaa;
constexpr u8 command_disable_keyboard = 0xad;
constexpr u8 command_enable_keyboard = 0xae;
constexpr u8 command_to_mouse = 0xd4;

constexpr u8 config_keyboard_interrupt = 1 << 0;
constexpr u8 config_mouse_interrupt = 1 << 1;
constexpr u8 config_keyboard_translation = 1 << 6;

constexpr u8 device_acknowledge = 0xfa;
constexpr u8 device_enable_reporting = 0xf4;
constexpr u8 device_defaults = 0xf6;
constexpr u8 device_set_sample_rate = 0xf3;
constexpr u8 device_get_id = 0xf2;

// Filled in interrupt context, on one processor at a time, and emptied by one thread.
constexpr size_t ring_capacity = 256;
u8 s_bytes[ring_capacity];
bool s_from_mouse[ring_capacity];
Atomic<u32> s_head { 0 };
Atomic<u32> s_tail { 0 };
WaitQueue s_arrived;

bool wait_to_write()
{
    for (int i = 0; i < 100'000; ++i) {
        if (!(Arch::port_read8(port_status) & status_input_full))
            return true;
        Arch::pause();
    }
    return false;
}

bool wait_to_read()
{
    for (int i = 0; i < 100'000; ++i) {
        if (Arch::port_read8(port_status) & status_output_full)
            return true;
        Arch::pause();
    }
    return false;
}

void send_command(u8 command)
{
    if (wait_to_write())
        Arch::port_write8(port_command, command);
}

void send_data(u8 data)
{
    if (wait_to_write())
        Arch::port_write8(port_data, data);
}

Optional<u8> read_response()
{
    if (!wait_to_read())
        return {};
    return Arch::port_read8(port_data);
}

void flush_output()
{
    for (int i = 0; i < 64 && (Arch::port_read8(port_status) & status_output_full); ++i)
        (void)Arch::port_read8(port_data);
}

bool send_to_mouse(u8 byte)
{
    send_command(command_to_mouse);
    send_data(byte);
    return read_response() == device_acknowledge;
}

void handle_interrupt(void*)
{
    while (Arch::port_read8(port_status) & status_output_full) {
        u8 const status = Arch::port_read8(port_status);
        u8 const byte = Arch::port_read8(port_data);
        u32 const head = s_head.load();
        // A full ring means nobody's looking: what's new can wait for what's old.
        if (head - s_tail.load() == ring_capacity)
            continue;
        s_bytes[head % ring_capacity] = byte;
        s_from_mouse[head % ring_capacity] = status & status_from_mouse;
        s_head.store(head + 1);
    }
    s_arrived.wake_all();
}

// Scan code set 1, which the controller translates everything into, is what the keyboard event numbers
// were taken from: up to here, they're the same. The rest come after an 0xe0.
constexpr u8 last_plain_code = 0x58;

u16 extended_key(u8 code)
{
    switch (code) {
    case 0x1c:
        return VULTURE_KEY_KPENTER;
    case 0x1d:
        return VULTURE_KEY_RIGHTCTRL;
    case 0x35:
        return VULTURE_KEY_KPSLASH;
    case 0x38:
        return VULTURE_KEY_RIGHTALT;
    case 0x47:
        return VULTURE_KEY_HOME;
    case 0x48:
        return VULTURE_KEY_UP;
    case 0x49:
        return VULTURE_KEY_PAGEUP;
    case 0x4b:
        return VULTURE_KEY_LEFT;
    case 0x4d:
        return VULTURE_KEY_RIGHT;
    case 0x4f:
        return VULTURE_KEY_END;
    case 0x50:
        return VULTURE_KEY_DOWN;
    case 0x51:
        return VULTURE_KEY_PAGEDOWN;
    case 0x52:
        return VULTURE_KEY_INSERT;
    case 0x53:
        return VULTURE_KEY_DELETE;
    case 0x5b:
        return VULTURE_KEY_LEFTMETA;
    case 0x5c:
        return VULTURE_KEY_RIGHTMETA;
    default:
        return 0;
    }
}

class KeyboardDecoder {
public:
    void take(u8 byte)
    {
        if (byte == 0xe0) {
            m_extended = true;
            return;
        }
        // The prefix of the Pause key, whose six bytes say nothing useful.
        if (byte == 0xe1) {
            m_ignoring = 5;
            return;
        }
        if (m_ignoring > 0) {
            --m_ignoring;
            return;
        }
        bool const released = byte & 0x80;
        u8 const code = byte & 0x7f;
        u16 const key = m_extended ? extended_key(code) : (code <= last_plain_code ? code : 0);
        m_extended = false;
        if (key == 0)
            return;
        // Repeats come as more presses; the event says so.
        bool const repeat = !released && m_held[key / 8] & (1 << (key % 8));
        if (released)
            m_held[key / 8] &= static_cast<u8>(~(1 << (key % 8)));
        else
            m_held[key / 8] |= static_cast<u8>(1 << (key % 8));
        post_input_event(VULTURE_EV_KEY, key, released ? 0 : (repeat ? 2 : 1));
        post_input_event(VULTURE_EV_SYN, 0, 0);
    }

private:
    bool m_extended { false };
    int m_ignoring { 0 };
    u8 m_held[32] {};
};

class MouseDecoder {
public:
    void set_has_wheel(bool has_wheel) { m_packet_size = has_wheel ? 4 : 3; }

    void take(u8 byte)
    {
        // The first byte of a packet always has this bit set, so that a lost byte can't go on
        // meaning the wrong thing for ever.
        if (m_count == 0 && !(byte & (1 << 3)))
            return;
        m_packet[m_count++] = byte;
        if (m_count < m_packet_size)
            return;
        m_count = 0;
        u8 const flags = m_packet[0];
        i32 const dx = static_cast<i32>(m_packet[1]) - ((flags & (1 << 4)) ? 256 : 0);
        i32 const dy = static_cast<i32>(m_packet[2]) - ((flags & (1 << 5)) ? 256 : 0);
        if (dx != 0)
            post_input_event(VULTURE_EV_REL, VULTURE_REL_X, dx);
        // Up is positive here, and down everywhere else.
        if (dy != 0)
            post_input_event(VULTURE_EV_REL, VULTURE_REL_Y, -dy);
        if (m_packet_size == 4 && m_packet[3] != 0)
            post_input_event(VULTURE_EV_REL, VULTURE_REL_WHEEL, -static_cast<i8>(m_packet[3]));
        u8 const buttons = flags & 7;
        u8 const changed = buttons ^ m_buttons;
        m_buttons = buttons;
        if (changed & 1)
            post_input_event(VULTURE_EV_KEY, VULTURE_BTN_LEFT, buttons & 1);
        if (changed & 2)
            post_input_event(VULTURE_EV_KEY, VULTURE_BTN_RIGHT, (buttons >> 1) & 1);
        if (changed & 4)
            post_input_event(VULTURE_EV_KEY, VULTURE_BTN_MIDDLE, (buttons >> 2) & 1);
        post_input_event(VULTURE_EV_SYN, 0, 0);
    }

private:
    u8 m_packet[4] {};
    size_t m_count { 0 };
    size_t m_packet_size { 3 };
    u8 m_buttons { 0 };
};

KeyboardDecoder s_keyboard;
MouseDecoder s_mouse;

void input_thread(void*)
{
    for (;;) {
        s_arrived.wait_until([] { return s_tail.load() != s_head.load(); });
        u32 const tail = s_tail.load();
        u8 const byte = s_bytes[tail % ring_capacity];
        bool const from_mouse = s_from_mouse[tail % ring_capacity];
        s_tail.store(tail + 1);
        if (from_mouse)
            s_mouse.take(byte);
        else
            s_keyboard.take(byte);
    }
}

}

void initialize_ps2_input()
{
    if (!ACPI::fixed_description().has_i8042) {
        dmesgln("Input: no PS/2 controller, by the firmware's tables");
        return;
    }
    // A controller that isn't there reads all ones.
    if (Arch::port_read8(port_status) == 0xff) {
        dmesgln("Input: no PS/2 controller");
        return;
    }
    send_command(command_disable_keyboard);
    send_command(command_disable_mouse);
    flush_output();
    send_command(command_read_config);
    auto const read_config = read_response();
    if (!read_config.has_value())
        return;
    u8 config = read_config.value();
    // Off for now, and keep translating to set 1.
    config = (config & ~(config_keyboard_interrupt | config_mouse_interrupt)) | config_keyboard_translation;
    send_command(command_write_config);
    send_data(config);
    send_command(command_self_test);
    if (read_response() != 0x55) {
        dmesgln("Input: the PS/2 controller failed its self test");
        return;
    }
    // (A self test can reset it, on some machines.)
    send_command(command_write_config);
    send_data(config);
    flush_output();

    send_command(command_enable_keyboard);
    send_data(device_enable_reporting);
    bool const has_keyboard = read_response() == device_acknowledge;

    send_command(command_enable_mouse);
    flush_output();
    bool has_mouse = send_to_mouse(device_defaults);
    bool has_wheel = false;
    if (has_mouse) {
        // Asks it to act as a wheel mouse: by a special sequence of sample rates, as there was no other way.
        constexpr u8 knock[] = { 200, 100, 80 };
        for (u8 rate : knock) {
            (void)send_to_mouse(device_set_sample_rate);
            (void)send_to_mouse(rate);
        }
        if (send_to_mouse(device_get_id))
            has_wheel = read_response() == 3;
        s_mouse.set_has_wheel(has_wheel);
        has_mouse = send_to_mouse(device_enable_reporting);
    }
    flush_output();

    auto thread = Thread::create("ps2"_sv, input_thread, nullptr, { .qos = QoS::UserInteractive });
    if (thread.is_error())
        return;
    thread.value()->start();

    if (has_keyboard) {
        if (auto vector = X86_64::allocate_isa_vector(1); !vector.is_error()) {
            register_interrupt_handler(vector.value(), handle_interrupt, nullptr);
            Arch::enable_interrupt(vector.value());
            config |= config_keyboard_interrupt;
        }
    }
    if (has_mouse) {
        if (auto vector = X86_64::allocate_isa_vector(12); !vector.is_error()) {
            register_interrupt_handler(vector.value(), handle_interrupt, nullptr);
            Arch::enable_interrupt(vector.value());
            config |= config_mouse_interrupt;
        }
    }
    // (Read while they were turned off, which it says in here too.)
    constexpr u8 config_ports_disabled = (1 << 4) | (1 << 5);
    config &= ~config_ports_disabled;
    send_command(command_write_config);
    send_data(config);
    dmesgln("Input: PS/2 {}{}{}", has_keyboard ? "keyboard"_sv : "no keyboard"_sv, has_mouse ? ", mouse"_sv : ", no mouse"_sv, has_wheel ? " with a wheel"_sv : ""_sv);
}

}
