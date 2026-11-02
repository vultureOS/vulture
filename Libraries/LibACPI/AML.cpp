/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibACPI/AML.h>

namespace ACPI {

namespace {

// Opcodes, as far as is needed to know how long things are.
constexpr u8 op_zero = 0x00;
constexpr u8 op_one = 0x01;
constexpr u8 op_alias = 0x06;
constexpr u8 op_name = 0x08;
constexpr u8 op_byte = 0x0a;
constexpr u8 op_word = 0x0b;
constexpr u8 op_dword = 0x0c;
constexpr u8 op_string = 0x0d;
constexpr u8 op_qword = 0x0e;
constexpr u8 op_scope = 0x10;
constexpr u8 op_buffer = 0x11;
constexpr u8 op_package = 0x12;
constexpr u8 op_var_package = 0x13;
constexpr u8 op_method = 0x14;
constexpr u8 op_extended = 0x5b;
constexpr u8 op_ones = 0xff;

constexpr u8 ext_mutex = 0x01;
constexpr u8 ext_event = 0x02;
constexpr u8 ext_op_region = 0x80;
constexpr u8 ext_field = 0x81;
constexpr u8 ext_device = 0x82;
constexpr u8 ext_processor = 0x83;
constexpr u8 ext_power_resource = 0x84;
constexpr u8 ext_thermal_zone = 0x85;
constexpr u8 ext_index_field = 0x86;
constexpr u8 ext_bank_field = 0x87;

constexpr u8 prefix_root = 0x5c;
constexpr u8 prefix_parent = 0x5e;
constexpr u8 prefix_dual = 0x2e;
constexpr u8 prefix_multi = 0x2f;

class Reader {
public:
    explicit Reader(ReadonlyBytes bytes, size_t at = 0, size_t end = 0)
        : m_bytes(bytes)
        , m_at(at)
        , m_end(end ? end : bytes.size())
    {
    }

    [[nodiscard]] bool at_end() const { return m_at >= m_end; }
    [[nodiscard]] size_t position() const { return m_at; }
    [[nodiscard]] size_t end() const { return m_end; }
    void go_to(size_t at) { m_at = min(at, m_end); }

    [[nodiscard]] bool peek(u8& byte) const
    {
        if (m_at >= m_end)
            return false;
        byte = m_bytes[m_at];
        return true;
    }

    [[nodiscard]] bool read(u8& byte)
    {
        if (!peek(byte))
            return false;
        ++m_at;
        return true;
    }

    // A length that covers itself; returns where what it measures ends.
    [[nodiscard]] Optional<size_t> read_package_end()
    {
        size_t const start = m_at;
        u8 first;
        if (!read(first))
            return {};
        size_t const extra = first >> 6;
        size_t length = extra == 0 ? (first & 0x3f) : (first & 0x0f);
        for (size_t i = 0; i < extra; ++i) {
            u8 byte;
            if (!read(byte))
                return {};
            length |= static_cast<size_t>(byte) << (4 + 8 * i);
        }
        size_t const end = start + length;
        if (end > m_end || end < m_at)
            return {};
        return end;
    }

    // A name's last segment; any path before it doesn't matter here.
    [[nodiscard]] bool read_name(u32& last_segment)
    {
        u8 byte;
        if (!peek(byte))
            return false;
        while (byte == prefix_root || byte == prefix_parent) {
            ++m_at;
            if (!peek(byte))
                return false;
        }
        size_t segments = 1;
        if (byte == prefix_dual) {
            ++m_at;
            segments = 2;
        } else if (byte == prefix_multi) {
            ++m_at;
            u8 count;
            if (!read(count))
                return false;
            segments = count;
        } else if (byte == 0) {
            ++m_at;
            last_segment = 0;
            return true;
        }
        for (size_t i = 0; i < segments; ++i) {
            if (m_at + 4 > m_end)
                return false;
            __builtin_memcpy(&last_segment, m_bytes.data() + m_at, 4);
            m_at += 4;
        }
        return true;
    }

    // A plain number, as it appears in a package; false if this is anything else (which is skipped).
    [[nodiscard]] bool read_integer(u64& value, bool& is_integer)
    {
        u8 op;
        if (!read(op))
            return false;
        is_integer = true;
        switch (op) {
        case op_zero:
            value = 0;
            return true;
        case op_one:
            value = 1;
            return true;
        case op_ones:
            value = ~0ull;
            return true;
        case op_byte:
            return read_little_endian(value, 1);
        case op_word:
            return read_little_endian(value, 2);
        case op_dword:
            return read_little_endian(value, 4);
        case op_qword:
            return read_little_endian(value, 8);
        default:
            is_integer = false;
            --m_at;
            return skip_package_element();
        }
    }

    [[nodiscard]] bool skip_package_element()
    {
        u8 op;
        if (!peek(op))
            return false;
        if (op == op_package || op == op_var_package || op == op_buffer) {
            ++m_at;
            auto const end = read_package_end();
            if (!end.has_value())
                return false;
            go_to(end.value());
            return true;
        }
        if (op == op_string) {
            ++m_at;
            u8 byte;
            do {
                if (!read(byte))
                    return false;
            } while (byte != 0);
            return true;
        }
        u64 ignored;
        bool is_integer = false;
        if (op == op_zero || op == op_one || op == op_ones || op == op_byte || op == op_word || op == op_dword || op == op_qword)
            return read_integer(ignored, is_integer);
        u32 name;
        return read_name(name);
    }

private:
    [[nodiscard]] bool read_little_endian(u64& value, size_t bytes)
    {
        value = 0;
        for (size_t i = 0; i < bytes; ++i) {
            u8 byte;
            if (!read(byte))
                return false;
            value |= static_cast<u64>(byte) << (8 * i);
        }
        return true;
    }

    ReadonlyBytes m_bytes;
    size_t m_at;
    size_t m_end;
};

constexpr u32 name_of(char const (&text)[5])
{
    return static_cast<u32>(static_cast<u8>(text[0])) | (static_cast<u32>(static_cast<u8>(text[1])) << 8)
        | (static_cast<u32>(static_cast<u8>(text[2])) << 16) | (static_cast<u32>(static_cast<u8>(text[3])) << 24);
}

// Goes through every Name in a term list, into every scope it can tell the length of.
template<typename Callback>
void walk_names(ReadonlyBytes table, size_t start, size_t end, Callback const& callback, unsigned depth = 0)
{
    if (depth > 32)
        return;
    Reader reader(table, start, end);
    while (!reader.at_end()) {
        u8 op;
        if (!reader.read(op))
            return;
        switch (op) {
        case op_name: {
            u32 name;
            if (!reader.read_name(name))
                return;
            size_t const value = reader.position();
            callback(name, value, reader.end());
            if (!reader.skip_package_element())
                return;
            break;
        }
        case op_alias: {
            u32 name;
            if (!reader.read_name(name) || !reader.read_name(name))
                return;
            break;
        }
        case op_scope: {
            auto const scope_end = reader.read_package_end();
            u32 name;
            if (!scope_end.has_value() || !reader.read_name(name))
                return;
            walk_names(table, reader.position(), scope_end.value(), callback, depth + 1);
            reader.go_to(scope_end.value());
            break;
        }
        case op_method: {
            auto const method_end = reader.read_package_end();
            if (!method_end.has_value())
                return;
            reader.go_to(method_end.value());
            break;
        }
        case op_extended: {
            u8 extended;
            if (!reader.read(extended))
                return;
            switch (extended) {
            case ext_device:
            case ext_thermal_zone: {
                auto const scope_end = reader.read_package_end();
                u32 name;
                if (!scope_end.has_value() || !reader.read_name(name))
                    return;
                walk_names(table, reader.position(), scope_end.value(), callback, depth + 1);
                reader.go_to(scope_end.value());
                break;
            }
            case ext_processor:
            case ext_power_resource:
            case ext_field:
            case ext_index_field:
            case ext_bank_field: {
                auto const scope_end = reader.read_package_end();
                if (!scope_end.has_value())
                    return;
                reader.go_to(scope_end.value());
                break;
            }
            case ext_mutex: {
                u32 name;
                u8 flags;
                if (!reader.read_name(name) || !reader.read(flags))
                    return;
                break;
            }
            case ext_event: {
                u32 name;
                if (!reader.read_name(name))
                    return;
                break;
            }
            case ext_op_region: {
                u32 name;
                u8 space;
                u64 number;
                bool is_integer;
                if (!reader.read_name(name) || !reader.read(space))
                    return;
                if (!reader.read_integer(number, is_integer) || !reader.read_integer(number, is_integer))
                    return;
                break;
            }
            default:
                // Something outside of what's known here: nothing after it can be trusted.
                return;
            }
            break;
        }
        default:
            // Code outside of a method, as an If around a piece of a table. This has no way to tell
            // how to get past it, and nothing it wants is ever there.
            return;
        }
    }
}

}

Optional<SleepType> find_sleep_type_s5(ReadonlyBytes table)
{
    Optional<SleepType> found;
    walk_names(table, 0, table.size(), [&](u32 name, size_t value, size_t end) {
        if (name != name_of("_S5_") || found.has_value())
            return;
        Reader reader(table, value, end);
        u8 op;
        if (!reader.read(op) || op != op_package)
            return;
        auto const package_end = reader.read_package_end();
        u8 count;
        if (!package_end.has_value() || !reader.read(count) || count < 1)
            return;
        u64 values[2] = { 0, 0 };
        bool is_integer = false;
        if (!reader.read_integer(values[0], is_integer) || !is_integer)
            return;
        if (count >= 2 && reader.position() < package_end.value() && !reader.read_integer(values[1], is_integer))
            return;
        found = SleepType { static_cast<u8>(values[0]), static_cast<u8>(values[1]) };
    });
    return found;
}

void find_interrupt_routes(ReadonlyBytes table, Vector<InterruptRoute>& routes)
{
    walk_names(table, 0, table.size(), [&](u32 name, size_t value, size_t end) {
        if (name != name_of("_PRT"))
            return;
        Reader reader(table, value, end);
        u8 op;
        if (!reader.read(op) || op != op_package)
            return;
        auto const package_end = reader.read_package_end();
        u8 count;
        if (!package_end.has_value() || !reader.read(count))
            return;
        for (u8 i = 0; i < count && reader.position() < package_end.value(); ++i) {
            u8 inner_op;
            if (!reader.peek(inner_op) || inner_op != op_package)
                return;
            (void)reader.read(inner_op);
            auto const entry_end = reader.read_package_end();
            u8 entries;
            if (!entry_end.has_value() || !reader.read(entries) || entries != 4)
                return;
            // { address, pin, source, source index }: address has the slot in its high half; the source
            // is 0 for a direct line, given by the index, or else the name of a link device.
            u64 address, pin, source_index = 0;
            bool is_integer = false;
            if (!reader.read_integer(address, is_integer) || !is_integer)
                return;
            if (!reader.read_integer(pin, is_integer) || !is_integer)
                return;
            u64 source = 0;
            bool source_is_integer = false;
            if (!reader.read_integer(source, source_is_integer))
                return;
            if (!reader.read_integer(source_index, is_integer) || !is_integer)
                return;
            InterruptRoute route {};
            route.slot = static_cast<u32>(address >> 16);
            route.pin = static_cast<u8>(pin);
            route.direct = source_is_integer && source == 0;
            route.line = static_cast<u32>(source_index);
            routes.append(route);
            reader.go_to(entry_end.value());
        }
    });
}

}
