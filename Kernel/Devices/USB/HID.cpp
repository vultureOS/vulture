/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/input.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Input/Input.h>
#include <Kernel/Devices/USB/USB.h>
#include <Kernel/Memory/Heap.h>
#include <LibBase/New.h>

// USB keyboards and pointers. A keyboard is asked to speak its simplest protocol, which every keyboard
// has to, as what's in a BIOS only knows that one. A mouse, or a tablet, says itself what its reports
// look like, in a report descriptor that's read here far enough to find its position and buttons.

namespace Kernel::USB {

namespace {

constexpr u8 request_set_idle = 0x0a;
constexpr u8 request_set_protocol = 0x0b;

constexpr u8 subclass_boot = 1;
constexpr u8 protocol_keyboard = 1;

// From what USB calls a key to what's used here: this is by position, and starts at A, which is 4.
constexpr u8 key_codes[] = {
    30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, // A to Z
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11,                                                                         // 1 to 0
    28, 1, 14, 15, 57, 12, 13, 26, 27, 43, 43, 39, 40, 41, 51, 52, 53, 58,                                  // Enter to Caps Lock
    59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88,                                                         // F1 to F12
    99, 70, 119, 110, 102, 104, 111, 107, 109, 106, 105, 108, 103, 69,                                      // Print Screen to Num Lock
    98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71, 72, 73, 82, 83,                                         // The number pad
};

constexpr u16 modifier_codes[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

class Keyboard {
public:
    void take_report(ReadonlyBytes report)
    {
        if (report.size() < 8)
            return;
        u8 const modifiers = report[0];
        u8 const changed = modifiers ^ m_modifiers;
        for (int i = 0; i < 8; ++i) {
            if (changed & (1 << i))
                post_input_event(VULTURE_EV_KEY, modifier_codes[i], (modifiers & (1 << i)) ? 1 : 0);
        }
        m_modifiers = modifiers;
        // What was down that isn't now.
        for (int i = 0; i < 6; ++i) {
            u8 const key = m_keys[i];
            if (key >= 4 && !contains(report, key))
                post(key, 0);
        }
        // And the other way.
        for (int i = 0; i < 6; ++i) {
            u8 const key = report[2 + i];
            if (key >= 4 && !was_down(key))
                post(key, 1);
        }
        for (int i = 0; i < 6; ++i)
            m_keys[i] = report[2 + i];
        post_input_event(VULTURE_EV_SYN, 0, 0);
    }

private:
    [[nodiscard]] static bool contains(ReadonlyBytes report, u8 key)
    {
        for (int i = 0; i < 6; ++i) {
            if (report[2 + i] == key)
                return true;
        }
        return false;
    }

    [[nodiscard]] bool was_down(u8 key) const
    {
        for (u8 known : m_keys) {
            if (known == key)
                return true;
        }
        return false;
    }

    static void post(u8 key, i32 value)
    {
        size_t const index = static_cast<size_t>(key) - 4;
        if (index < sizeof(key_codes))
            post_input_event(VULTURE_EV_KEY, key_codes[index], value);
    }

    u8 m_modifiers { 0 };
    u8 m_keys[6] {};
};

struct Field {
    bool present { false };
    u32 bit_offset { 0 };
    u32 bits { 0 };
    bool is_relative { false };
    i32 minimum { 0 };
    i32 maximum { 0 };
};

class Pointer {
public:
    // Finds what it needs in what the device says its reports look like.
    void read_report_descriptor(ReadonlyBytes descriptor)
    {
        u32 usage_page = 0;
        i32 logical_minimum = 0;
        i32 logical_maximum = 0;
        u32 report_size = 0;
        u32 report_count = 0;
        u8 report_id = 0;
        u32 offset = 0;
        u32 usages[16];
        u32 usage_count = 0;
        u32 usage_minimum = 0;
        u32 usage_maximum = 0;

        size_t at = 0;
        while (at < descriptor.size()) {
            u8 const prefix = descriptor[at++];
            // A long item, which nothing uses.
            if (prefix == 0xfe) {
                if (at >= descriptor.size())
                    return;
                at += 2u + descriptor[at];
                continue;
            }
            size_t const size = (prefix & 3) == 3 ? 4 : (prefix & 3);
            if (at + size > descriptor.size())
                return;
            u32 value = 0;
            for (size_t i = 0; i < size; ++i)
                value |= static_cast<u32>(descriptor[at + i]) << (8 * i);
            i32 signed_value = static_cast<i32>(value);
            if (size == 1)
                signed_value = static_cast<i8>(value);
            else if (size == 2)
                signed_value = static_cast<i16>(value);
            at += size;

            u8 const type = (prefix >> 2) & 3;
            u8 const tag = prefix >> 4;
            if (type == 1) { // Global.
                switch (tag) {
                case 0:
                    usage_page = value;
                    break;
                case 1:
                    logical_minimum = signed_value;
                    break;
                case 2:
                    logical_maximum = signed_value;
                    break;
                case 7:
                    report_size = value;
                    break;
                case 8:
                    report_id = static_cast<u8>(value);
                    if (m_report_id == 0) {
                        m_report_id = report_id;
                        offset = 8;
                    } else if (report_id != m_report_id) {
                        m_other_reports = true;
                    } else {
                        m_other_reports = false;
                    }
                    break;
                case 9:
                    report_count = value;
                    break;
                default:
                    break;
                }
            } else if (type == 2) { // Local: these are for the next main item.
                switch (tag) {
                case 0:
                    if (usage_count < 16)
                        usages[usage_count++] = value;
                    break;
                case 1:
                    usage_minimum = value;
                    break;
                case 2:
                    usage_maximum = value;
                    break;
                default:
                    break;
                }
            } else if (type == 0) { // Main.
                if (tag == 8) { // Input.
                    bool const is_constant = value & 1;
                    bool const is_relative = value & 4;
                    if (!m_other_reports && !is_constant) {
                        for (u32 i = 0; i < report_count; ++i) {
                            u32 const field_offset = offset + i * report_size;
                            u32 usage = 0;
                            if (usage_count > 0)
                                usage = usages[i < usage_count ? i : usage_count - 1];
                            else if (usage_minimum + i <= usage_maximum)
                                usage = usage_minimum + i;
                            if (usage_page == 0x09) { // Buttons.
                                if (usage >= 1 && usage <= 3 && !m_buttons[usage - 1].present)
                                    m_buttons[usage - 1] = { true, field_offset, report_size, false, 0, 1 };
                            } else if (usage_page == 0x01) { // The generic desktop's.
                                Field* field = nullptr;
                                if (usage == 0x30)
                                    field = &m_x;
                                else if (usage == 0x31)
                                    field = &m_y;
                                else if (usage == 0x38)
                                    field = &m_wheel;
                                if (field && !field->present)
                                    *field = { true, field_offset, report_size, is_relative, logical_minimum, logical_maximum };
                            }
                        }
                    }
                    if (!m_other_reports)
                        offset += report_count * report_size;
                }
                usage_count = 0;
                usage_minimum = 0;
                usage_maximum = 0;
            }
        }
    }

    [[nodiscard]] bool has_position() const { return m_x.present && m_y.present; }
    [[nodiscard]] bool is_absolute() const { return !m_x.is_relative; }

    void take_report(ReadonlyBytes report)
    {
        if (m_report_id != 0 && (report.is_empty() || report[0] != m_report_id))
            return;
        if (m_x.present && m_y.present) {
            i32 const x = read(report, m_x);
            i32 const y = read(report, m_y);
            if (m_x.is_relative) {
                if (x != 0)
                    post_input_event(VULTURE_EV_REL, VULTURE_REL_X, x);
                if (y != 0)
                    post_input_event(VULTURE_EV_REL, VULTURE_REL_Y, y);
            } else {
                post_input_event(VULTURE_EV_ABS, VULTURE_ABS_X, scaled(x, m_x));
                post_input_event(VULTURE_EV_ABS, VULTURE_ABS_Y, scaled(y, m_y));
            }
        }
        if (m_wheel.present) {
            i32 const wheel = read(report, m_wheel);
            if (wheel != 0)
                post_input_event(VULTURE_EV_REL, VULTURE_REL_WHEEL, wheel);
        }
        static constexpr u16 button_codes[3] = { VULTURE_BTN_LEFT, VULTURE_BTN_RIGHT, VULTURE_BTN_MIDDLE };
        for (int i = 0; i < 3; ++i) {
            if (!m_buttons[i].present)
                continue;
            bool const pressed = read(report, m_buttons[i]) != 0;
            if (pressed != m_pressed[i]) {
                m_pressed[i] = pressed;
                post_input_event(VULTURE_EV_KEY, button_codes[i], pressed ? 1 : 0);
            }
        }
        post_input_event(VULTURE_EV_SYN, 0, 0);
    }

private:
    [[nodiscard]] static i32 read(ReadonlyBytes report, Field const& field)
    {
        u32 value = 0;
        for (u32 bit = 0; bit < field.bits && bit < 32; ++bit) {
            u32 const at = field.bit_offset + bit;
            if (at / 8 >= report.size())
                return 0;
            if (report[at / 8] & (1 << (at % 8)))
                value |= 1u << bit;
        }
        // Signed, if what it's said to go down to says it is.
        if (field.minimum < 0 && field.bits < 32 && (value & (1u << (field.bits - 1))))
            value |= ~0u << field.bits;
        return static_cast<i32>(value);
    }

    [[nodiscard]] static i32 scaled(i32 value, Field const& field)
    {
        i64 const range = static_cast<i64>(field.maximum) - field.minimum;
        if (range <= 0)
            return 0;
        i64 const position = static_cast<i64>(value) - field.minimum;
        return static_cast<i32>(position * VULTURE_INPUT_ABSOLUTE_MAXIMUM / range);
    }

    u8 m_report_id { 0 };
    bool m_other_reports { false };
    Field m_x;
    Field m_y;
    Field m_wheel;
    Field m_buttons[3];
    bool m_pressed[3] {};
};

Optional<EndpointDescription> interrupt_in_of(InterfaceDescription const& interface)
{
    for (u8 i = 0; i < interface.endpoint_count; ++i) {
        auto const& endpoint = interface.endpoints[i];
        if (endpoint.kind == EndpointKind::Interrupt && endpoint.is_in())
            return endpoint;
    }
    return {};
}

}

ErrorOr<bool> attach_hid(Device& device, InterfaceDescription const& interface)
{
    auto const in = interrupt_in_of(interface);
    if (!in.has_value())
        return false;
    constexpr u8 to_interface_of_class = 0x21;
    constexpr u8 from_interface_standard = 0x81;
    // Only when something has changed, for what can say.
    (void)device.control(to_interface_of_class, request_set_idle, 0, interface.number, {});

    if (interface.subclass == subclass_boot && interface.protocol == protocol_keyboard) {
        (void)TRY(device.control(to_interface_of_class, request_set_protocol, 0, interface.number, {}));
        auto* endpoint = TRY(device.open_endpoint(in.value()));
        auto* keyboard = new (Base::nothrow) Keyboard;
        if (!keyboard)
            return Error::from_errno(ENOMEM);
        TRY(device.poll_interrupts(*endpoint, 8, [](void* context, ReadonlyBytes report) { static_cast<Keyboard*>(context)->take_report(report); }, keyboard));
        dmesgln("USB: a keyboard");
        return true;
    }

    size_t const length = interface.report_descriptor_length;
    if (length == 0 || length > 4096)
        return false;
    auto* descriptor = static_cast<u8*>(kmalloc(length));
    if (!descriptor)
        return Error::from_errno(ENOMEM);
    auto const read = device.control(from_interface_standard, request_get_descriptor, descriptor_report << 8, interface.number, { descriptor, length });
    if (read.is_error()) {
        kfree(descriptor);
        return read.error();
    }
    auto* pointer = new (Base::nothrow) Pointer;
    if (!pointer) {
        kfree(descriptor);
        return Error::from_errno(ENOMEM);
    }
    pointer->read_report_descriptor({ descriptor, read.value() });
    kfree(descriptor);
    if (!pointer->has_position()) {
        delete pointer;
        return false;
    }
    auto* endpoint = TRY(device.open_endpoint(in.value()));
    TRY(device.poll_interrupts(*endpoint, min<size_t>(in->max_packet, 64), [](void* context, ReadonlyBytes report) { static_cast<Pointer*>(context)->take_report(report); }, pointer));
    dmesgln("USB: a pointer ({})", pointer->is_absolute() ? "absolute"_sv : "relative"_sv);
    return true;
}

}
