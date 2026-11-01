/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/input.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Input/Input.h>
#include <Kernel/FileSystem/DevFS.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/TTY/Terminals.h>
#include <Kernel/Time/Time.h>
#include <LibBase/New.h>
#include <LibBase/StringView.h>

// /dev/input, and the console's keyboard: until someone (the window server)
// opens /dev/input, keys become characters for /dev/console, on a US layout.

namespace Kernel {

namespace {

void note_console_key(u16 code, i32 value);

class InputClient;

SpinLock s_lock;
InputClient* s_client { nullptr };

// One open of /dev/input.
class InputClient final : public Device {
public:
    InputClient()
        : Device(InodeType::CharacterDevice, 0600, device_number(13, 1))
    {
    }

    ~InputClient() override
    {
        SpinLockGuard guard(s_lock);
        s_client = nullptr;
    }

    ErrorOr<size_t> read(u64, size_t count, IOBuffer const& buffer) override
    {
        size_t room = count / sizeof(vulture_input_event);
        if (room == 0)
            return Error::from_errno(EINVAL);
        vulture_input_event chunk[64];
        size_t taken = 0;
        {
            SpinLockGuard guard(s_lock);
            while (taken < min(room, array_size(chunk)) && m_count > 0) {
                chunk[taken++] = m_events[m_head];
                m_head = (m_head + 1) % capacity;
                --m_count;
            }
        }
        if (taken == 0)
            return Error::from_errno(EAGAIN);
        TRY(buffer.write(0, { reinterpret_cast<u8 const*>(chunk), taken * sizeof(vulture_input_event) }));
        return taken * sizeof(vulture_input_event);
    }

    ErrorOr<size_t> write(u64, size_t, IOBuffer const&) override { return Error::from_errno(EINVAL); }

    [[nodiscard]] bool can_read() const override
    {
        SpinLockGuard guard(s_lock);
        return m_count > 0;
    }

    // With s_lock held. When the reader falls far behind, the oldest go.
    void push(vulture_input_event const& event)
    {
        if (m_count == capacity) {
            m_head = (m_head + 1) % capacity;
            --m_count;
        }
        m_events[(m_head + m_count) % capacity] = event;
        ++m_count;
    }

private:
    static constexpr size_t capacity = 1024;
    vulture_input_event m_events[capacity];
    size_t m_head { 0 };
    size_t m_count { 0 };
};

// /dev/input itself: what open() gives is a client, one at a time.
class InputDevice final : public Device {
public:
    InputDevice()
        : Device(InodeType::CharacterDevice, 0600, device_number(13, 0))
    {
    }

    ErrorOr<size_t> read(u64, size_t, IOBuffer const&) override { return Error::from_errno(EINVAL); }
    ErrorOr<size_t> write(u64, size_t, IOBuffer const&) override { return Error::from_errno(EINVAL); }

    ErrorOr<NonnullRefPtr<Inode>> open_inode(int) override
    {
        auto* client = new (Base::nothrow) InputClient;
        if (!client)
            return Error::from_errno(ENOMEM);
        auto reference = adopt_ref(*client);
        {
            SpinLockGuard guard(s_lock);
            if (s_client)
                return Error::from_errno(EBUSY);
            s_client = client;
        }
        return NonnullRefPtr<Inode>(*client);
    }
};

// --- The console's keyboard ---

struct ModifierState {
    bool shift { false };
    bool control { false };
    bool option { false };
    bool caps_lock { false };
} s_modifiers;

// By key code: the plain character, and with shift.
struct KeyPair {
    char plain;
    char shifted;
};

constexpr KeyPair us_layout[] = {
    /*  0 */ { 0, 0 }, { 0, 0 }, { '1', '!' }, { '2', '@' }, { '3', '#' }, { '4', '$' }, { '5', '%' }, { '6', '^' },
    /*  8 */ { '7', '&' }, { '8', '*' }, { '9', '(' }, { '0', ')' }, { '-', '_' }, { '=', '+' }, { 0, 0 }, { '\t', '\t' },
    /* 16 */ { 'q', 'Q' }, { 'w', 'W' }, { 'e', 'E' }, { 'r', 'R' }, { 't', 'T' }, { 'y', 'Y' }, { 'u', 'U' }, { 'i', 'I' },
    /* 24 */ { 'o', 'O' }, { 'p', 'P' }, { '[', '{' }, { ']', '}' }, { 0, 0 }, { 0, 0 }, { 'a', 'A' }, { 's', 'S' },
    /* 32 */ { 'd', 'D' }, { 'f', 'F' }, { 'g', 'G' }, { 'h', 'H' }, { 'j', 'J' }, { 'k', 'K' }, { 'l', 'L' }, { ';', ':' },
    /* 40 */ { '\'', '"' }, { '`', '~' }, { 0, 0 }, { '\\', '|' }, { 'z', 'Z' }, { 'x', 'X' }, { 'c', 'C' }, { 'v', 'V' },
    /* 48 */ { 'b', 'B' }, { 'n', 'N' }, { 'm', 'M' }, { ',', '<' }, { '.', '>' }, { '/', '?' }, { 0, 0 }, { '*', '*' },
    /* 56 */ { 0, 0 }, { ' ', ' ' },
};

// The keypad, with Num Lock's digits (macOS's keypad has only those).
constexpr char keypad[] = { '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.' };

void type_text(StringView text)
{
    for (size_t i = 0; i < text.length(); ++i)
        queue_console_input(static_cast<u8>(text[i]));
}

// What a terminal sends for the keys with no character.
StringView sequence_for(u16 code)
{
    switch (code) {
    case VULTURE_KEY_ESC:
        return "\x1b"_sv;
    case VULTURE_KEY_UP:
        return "\x1b[A"_sv;
    case VULTURE_KEY_DOWN:
        return "\x1b[B"_sv;
    case VULTURE_KEY_RIGHT:
        return "\x1b[C"_sv;
    case VULTURE_KEY_LEFT:
        return "\x1b[D"_sv;
    case VULTURE_KEY_HOME:
        return "\x1b[H"_sv;
    case VULTURE_KEY_END:
        return "\x1b[F"_sv;
    case VULTURE_KEY_INSERT:
        return "\x1b[2~"_sv;
    case VULTURE_KEY_DELETE:
        return "\x1b[3~"_sv;
    case VULTURE_KEY_PAGEUP:
        return "\x1b[5~"_sv;
    case VULTURE_KEY_PAGEDOWN:
        return "\x1b[6~"_sv;
    case VULTURE_KEY_F1:
        return "\x1bOP"_sv;
    case VULTURE_KEY_F1 + 1:
        return "\x1bOQ"_sv;
    case VULTURE_KEY_F1 + 2:
        return "\x1bOR"_sv;
    case VULTURE_KEY_F1 + 3:
        return "\x1bOS"_sv;
    case VULTURE_KEY_F1 + 4:
        return "\x1b[15~"_sv;
    case VULTURE_KEY_F1 + 5:
        return "\x1b[17~"_sv;
    case VULTURE_KEY_F1 + 6:
        return "\x1b[18~"_sv;
    case VULTURE_KEY_F1 + 7:
        return "\x1b[19~"_sv;
    case VULTURE_KEY_F1 + 8:
        return "\x1b[20~"_sv;
    case VULTURE_KEY_F10:
        return "\x1b[21~"_sv;
    case VULTURE_KEY_F11:
        return "\x1b[23~"_sv;
    case VULTURE_KEY_F12:
        return "\x1b[24~"_sv;
    default:
        return {};
    }
}

void note_console_key(u16 code, i32 value)
{
    bool pressed = value != 0;
    switch (code) {
    case VULTURE_KEY_LEFTSHIFT:
    case VULTURE_KEY_RIGHTSHIFT:
        s_modifiers.shift = pressed;
        return;
    case VULTURE_KEY_LEFTCTRL:
    case VULTURE_KEY_RIGHTCTRL:
        s_modifiers.control = pressed;
        return;
    case VULTURE_KEY_LEFTALT:
    case VULTURE_KEY_RIGHTALT:
        s_modifiers.option = pressed;
        return;
    case VULTURE_KEY_CAPSLOCK:
        if (value == 1)
            s_modifiers.caps_lock = !s_modifiers.caps_lock;
        return;
    default:
        break;
    }
    if (!pressed)
        return;

    if (code == VULTURE_KEY_ENTER || code == VULTURE_KEY_KPENTER) {
        queue_console_input('\r');
        return;
    }
    if (code == VULTURE_KEY_BACKSPACE) {
        queue_console_input(0x7f);
        return;
    }
    if (auto sequence = sequence_for(code); !sequence.is_empty()) {
        type_text(sequence);
        return;
    }
    char character = 0;
    if (code == VULTURE_KEY_KPSLASH) {
        character = '/';
    } else if (code >= VULTURE_KEY_KP7 && code <= VULTURE_KEY_KPDOT) {
        character = keypad[code - VULTURE_KEY_KP7];
    } else if (code < array_size(us_layout)) {
        auto const& pair = us_layout[code];
        bool is_letter = pair.plain >= 'a' && pair.plain <= 'z';
        bool upper = s_modifiers.shift != (s_modifiers.caps_lock && is_letter);
        character = upper ? pair.shifted : pair.plain;
    }
    if (!character)
        return;
    // Control maps onto the control characters, as terminals do.
    if (s_modifiers.control) {
        if (character >= 'a' && character <= 'z')
            character = static_cast<char>(character - 'a' + 1);
        else if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 1);
        else if (character == '[' || character == '{')
            character = 0x1b;
        else if (character == '\\' || character == '|')
            character = 0x1c;
        else if (character == ']' || character == '}')
            character = 0x1d;
        else if (character == ' ' || character == '@' || character == '2')
            character = 0;
        else
            return;
        queue_console_input(static_cast<u8>(character));
        return;
    }
    // Option is Meta here, as in Terminal with "Use Option as Meta key".
    if (s_modifiers.option)
        queue_console_input(0x1b);
    queue_console_input(static_cast<u8>(character));
}

}

void post_input_event(u16 type, u16 code, i32 value)
{
    bool to_console = false;
    bool queued = false;
    {
        SpinLockGuard guard(s_lock);
        if (s_client) {
            vulture_input_event event {};
            event.type = type;
            event.code = code;
            event.value = value;
            event.time = Time::microseconds_from_ticks(Time::now());
            s_client->push(event);
            queued = true;
        } else {
            to_console = type == VULTURE_EV_KEY;
        }
    }
    if (queued)
        note_poll_change();
    if (to_console)
        note_console_key(code, value);
}

void initialize_virtio_input();

void initialize_input()
{
    auto* device = new (Base::nothrow) InputDevice;
    if (!device) {
        dmesgln("Input: out of memory");
        return;
    }
    if (auto added = DevFS::the().add_device("input"_sv, adopt_ref(*static_cast<Inode*>(device))); added.is_error())
        dmesgln("Input: {}", added.error());
    initialize_virtio_input();
}

}
