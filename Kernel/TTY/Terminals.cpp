/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#if defined(__x86_64__)
#    include <Kernel/Arch/x86_64/APIC.h>
#endif
#include <Kernel/Devices/Serial/BootSerial.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/TTY/Terminals.h>
#include <Kernel/Tasks/Thread.h>
#include <LibBase/Format.h>
#include <LibBase/New.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

namespace Kernel {

namespace {

// /dev/console: the screen (serial port and framebuffer, through the
// kernel log), the serial port's input and the keyboard's (Devices/Input).
class ConsoleTTY final : public TTY {
public:
    ConsoleTTY()
        : TTY(device_number(5, 1), "console"_sv)
    {
    }

    // From the UART's interrupt: queued for the input thread, which runs
    // the line discipline (it may sleep and send signals).
    void queue_input(u8 byte)
    {
        {
            SpinLockGuard guard(m_queue_lock);
            if (m_queue_count < sizeof(m_queue)) {
                m_queue[(m_queue_head + m_queue_count) % sizeof(m_queue)] = byte;
                ++m_queue_count;
            }
        }
        m_queue_wait.wake_all();
    }

    [[noreturn]] void run_input()
    {
        for (;;) {
            m_queue_wait.wait_until([&] {
                SpinLockGuard guard(m_queue_lock);
                return m_queue_count > 0;
            });
            u8 bytes[sizeof(m_queue)];
            size_t count = 0;
            {
                SpinLockGuard guard(m_queue_lock);
                while (m_queue_count > 0) {
                    bytes[count++] = m_queue[m_queue_head];
                    m_queue_head = (m_queue_head + 1) % sizeof(m_queue);
                    --m_queue_count;
                }
            }
            receive({ bytes, count });
        }
    }

protected:
    void emit(ReadonlyBytes bytes, bool) override
    {
        kernel_log().write_raw({ reinterpret_cast<char const*>(bytes.data()), bytes.size() });
    }

private:
    SpinLock m_queue_lock;
    u8 m_queue[512];
    size_t m_queue_head { 0 };
    size_t m_queue_count { 0 };
    WaitQueue m_queue_wait;
};

class PseudoTerminalMaster;

// The program side of a pseudo-terminal: /dev/ttys000 and on, as macOS
// names them.
// What the slave's programs have written, until the master reads it. Its own object, as
// a program can be waiting for room in it just as the master goes away.
class PseudoTerminalOutput final : public RefCounted<PseudoTerminalOutput> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<PseudoTerminalOutput>> create()
    {
        auto* output = new (Base::nothrow) PseudoTerminalOutput;
        if (!output)
            return Error::from_errno(ENOMEM);
        return adopt_ref(*output);
    }

    // What there is; waiting for some if there is none. 0 at the end: the slave was open,
    // and everything that had it has closed it.
    ErrorOr<size_t> read(size_t count, IOBuffer const& buffer)
    {
        if (count == 0)
            return 0;
        u8 chunk[1024];
        size_t taken = 0;
        for (;;) {
            {
                SpinLockGuard guard(m_lock);
                while (taken < min(count, sizeof(chunk)) && m_count > 0) {
                    chunk[taken++] = m_output[m_head];
                    m_head = (m_head + 1) % sizeof(m_output);
                    --m_count;
                }
                if (taken == 0 && slave_gone_locked())
                    return 0;
            }
            if (taken > 0)
                break;
            if (!m_readable.wait_until_interruptible([&] { return can_read(); }))
                return Error::from_errno(EINTR);
        }
        m_writable.wake_all();
        notify_pollers();
        TRY(buffer.write(0, { chunk, taken }));
        return taken;
    }

    [[nodiscard]] bool can_read() const
    {
        SpinLockGuard guard(m_lock);
        return m_count > 0 || slave_gone_locked();
    }

    [[nodiscard]] bool slave_has_gone() const
    {
        SpinLockGuard guard(m_lock);
        return slave_gone_locked();
    }

    // Program output, waiting for room unless it is echo (which runs inside the master's
    // own write and must not wait on it). With no master, it goes nowhere.
    void deliver(ReadonlyBytes bytes, bool from_echo)
    {
        size_t done = 0;
        while (done < bytes.size()) {
            size_t copied = 0;
            {
                SpinLockGuard guard(m_lock);
                if (m_master_closed)
                    return;
                while (done + copied < bytes.size() && m_count < sizeof(m_output)) {
                    m_output[(m_head + m_count) % sizeof(m_output)] = bytes[done + copied];
                    ++m_count;
                    ++copied;
                }
            }
            done += copied;
            if (copied > 0) {
                m_readable.wake_all();
                notify_pollers();
            }
            if (done < bytes.size()) {
                if (from_echo)
                    return;
                if (!m_writable.wait_until_interruptible([&] {
                        SpinLockGuard guard(m_lock);
                        return m_count < sizeof(m_output) || m_master_closed;
                    }))
                    return;
            }
        }
    }

    void master_closed()
    {
        {
            SpinLockGuard guard(m_lock);
            m_master_closed = true;
        }
        m_writable.wake_all();
    }

    void slave_opened()
    {
        SpinLockGuard guard(m_lock);
        ++m_slave_descriptions;
        m_slave_was_opened = true;
    }

    void slave_closed()
    {
        {
            SpinLockGuard guard(m_lock);
            --m_slave_descriptions;
            if (m_slave_descriptions > 0)
                return;
        }
        m_readable.wake_all();
        notify_pollers();
    }

private:
    PseudoTerminalOutput() = default;

    [[nodiscard]] bool slave_gone_locked() const { return m_slave_was_opened && m_slave_descriptions == 0; }

    mutable SpinLock m_lock;
    u8 m_output[16 * KiB];
    size_t m_head { 0 };
    size_t m_count { 0 };
    u32 m_slave_descriptions { 0 };
    bool m_slave_was_opened { false };
    bool m_master_closed { false };
    WaitQueue m_readable;
    WaitQueue m_writable;
};

class PseudoTerminalSlave final : public TTY {
public:
    PseudoTerminalSlave(u32 number, NonnullRefPtr<PseudoTerminalOutput> output, StringView name)
        : TTY(device_number(16, number), name)
        , m_number(number)
        , m_output(move(output))
    {
    }

    [[nodiscard]] u32 number() const { return m_number; }
    void master_closed();

    void description_opened() override { m_output->slave_opened(); }
    void description_closed() override { m_output->slave_closed(); }

protected:
    void emit(ReadonlyBytes, bool from_echo) override;

private:
    u32 m_number;
    NonnullRefPtr<PseudoTerminalOutput> m_output;
};

// The terminal-window side: what the slave's programs write comes out of
// its read(); what it writes is typed into the slave.
class PseudoTerminalMaster final : public Device {
public:
    PseudoTerminalMaster(u32 number, NonnullRefPtr<PseudoTerminalOutput> output)
        : Device(InodeType::CharacterDevice, 0620, device_number(15, number))
        , m_number(number)
        , m_output(move(output))
    {
    }

    ~PseudoTerminalMaster() override
    {
        m_output->master_closed();
        if (m_slave) {
            m_slave->master_closed();
            DevFS::the().remove_device(m_slave->tty_name());
        }
        release_number(m_number);
    }

    void set_slave(NonnullRefPtr<PseudoTerminalSlave> slave) { m_slave = move(slave); }

    ErrorOr<size_t> read(u64, size_t count, IOBuffer const& buffer) override
    {
        return m_output->read(count, buffer);
    }

    ErrorOr<size_t> write(u64, size_t count, IOBuffer const& buffer) override
    {
        size_t done = 0;
        while (done < count) {
            u8 chunk[256];
            size_t length = min(count - done, sizeof(chunk));
            TRY(buffer.read(done, { chunk, length }));
            m_slave->receive({ chunk, length });
            done += length;
        }
        return done;
    }

    ErrorOr<FlatPtr> ioctl(FileDescription& description, unsigned request, FlatPtr argument) override
    {
        switch (request) {
        case TIOCGPTN:
            TRY(copy_to_user(argument, static_cast<unsigned>(m_number)));
            return 0;
        case TIOCSPTLCK:
            return 0;
        case TIOCGWINSZ:
        case TIOCSWINSZ:
            return m_slave->ioctl(description, request, argument);
        default:
            return Error::from_errno(ENOTTY);
        }
    }

    [[nodiscard]] bool can_read() const override { return m_output->can_read(); }
    [[nodiscard]] bool can_write() const override { return true; }
    [[nodiscard]] bool is_hung_up() const override { return m_output->slave_has_gone(); }

    static ErrorOr<u32> allocate_number();
    static void release_number(u32);

private:
    u32 m_number;
    NonnullRefPtr<PseudoTerminalOutput> m_output;
    RefPtr<PseudoTerminalSlave> m_slave;
};

constexpr u32 max_pseudo_terminals = 256;
SpinLock s_numbers_lock;
bool s_numbers_used[max_pseudo_terminals];

ErrorOr<u32> PseudoTerminalMaster::allocate_number()
{
    SpinLockGuard guard(s_numbers_lock);
    for (u32 number = 0; number < max_pseudo_terminals; ++number) {
        if (!s_numbers_used[number]) {
            s_numbers_used[number] = true;
            return number;
        }
    }
    return Error::from_errno(ENOSPC);
}

void PseudoTerminalMaster::release_number(u32 number)
{
    SpinLockGuard guard(s_numbers_lock);
    s_numbers_used[number] = false;
}

void PseudoTerminalSlave::emit(ReadonlyBytes bytes, bool from_echo)
{
    m_output->deliver(bytes, from_echo);
}

void PseudoTerminalSlave::master_closed()
{
    hang_up();
}

// As in /etc/group.
static constexpr u32 tty_group = 4;

// /dev/ptmx: each open makes a new pseudo-terminal and opens its master.
class PseudoTerminalMultiplexer final : public Device {
public:
    PseudoTerminalMultiplexer()
        : Device(InodeType::CharacterDevice, 0666, device_number(5, 2))
    {
    }

    ErrorOr<NonnullRefPtr<Inode>> open_inode(int) override
    {
        auto output = TRY(PseudoTerminalOutput::create());
        u32 number = TRY(PseudoTerminalMaster::allocate_number());
        auto* master = new (Base::nothrow) PseudoTerminalMaster(number, output);
        if (!master) {
            PseudoTerminalMaster::release_number(number);
            return Error::from_errno(ENOMEM);
        }
        auto master_reference = adopt_ref(*master);
        FixedStringBuilder<16> name;
        format_to(name, "ttys{:03}", number);
        auto* slave = new (Base::nothrow) PseudoTerminalSlave(number, output, name.view());
        if (!slave)
            return Error::from_errno(ENOMEM);
        auto slave_reference = adopt_ref(*slave);
        // It belongs to whoever opened it (grantpt's work, done here): owner may read and
        // write it, the tty group may only write to it, nobody else may touch it.
        if (auto const* credentials = current_credentials())
            MUST(slave->set_owner(credentials->uid, tty_group));
        TRY(DevFS::the().add_device(name.view(), NonnullRefPtr<Inode>(*static_cast<Inode*>(slave))));
        master->set_slave(move(slave_reference));
        return NonnullRefPtr<Inode>(*static_cast<Inode*>(master));
    }
};

RefPtr<ConsoleTTY> s_console;

#if defined(__x86_64__)
// COM1's: an ISA interrupt.
Optional<u32> serial_interrupt()
{
    auto vector = X86_64::allocate_isa_vector(4);
    if (vector.is_error())
        return {};
    return vector.value();
}
#else
// The PL011's interrupt, from the device tree.
Optional<u32> serial_interrupt()
{
    auto tree = DeviceTree::FlattenedDeviceTree::parse(memory_manager().device_tree());
    if (tree.is_error())
        return {};
    auto node = tree.value().find_stdout_node();
    if (!node.has_value())
        node = tree.value().find_compatible_node("arm,pl011"_sv);
    if (!node.has_value())
        return {};
    auto interrupts = tree.value().find_property(node.value(), "interrupts"_sv);
    if (!interrupts.has_value() || interrupts->cell_count() < 2)
        return {};
    auto type = interrupts->read_cells(0, 1);
    auto number = interrupts->read_cells(1, 1);
    if (!type.has_value() || !number.has_value())
        return {};
    // GIC: shared peripheral interrupts from 32, private ones from 16.
    return static_cast<u32>(number.value() + (type.value() == 0 ? 32 : 16));
}
#endif

}

ErrorOr<void> create_terminals()
{
    auto* console = new (Base::nothrow) ConsoleTTY;
    if (!console)
        return Error::from_errno(ENOMEM);
    s_console = adopt_ref(*console);
    TRY(DevFS::the().add_device("console"_sv, NonnullRefPtr<Inode>(*static_cast<Inode*>(console))));
    auto* multiplexer = new (Base::nothrow) PseudoTerminalMultiplexer;
    if (!multiplexer)
        return Error::from_errno(ENOMEM);
    TRY(DevFS::the().add_device("ptmx"_sv, adopt_ref(*static_cast<Inode*>(multiplexer))));

    auto thread = TRY(Thread::create("console-input"_sv, [](void* context) { static_cast<ConsoleTTY*>(context)->run_input(); }, console, { .qos = QoS::UserInteractive }));
    thread->start();
    if (auto interrupt = serial_interrupt(); interrupt.has_value() && boot_serial().is_present())
        boot_serial().enable_receive_interrupt(interrupt.value(), [](void* context, u8 byte) { static_cast<ConsoleTTY*>(context)->queue_input(byte); }, console);
    else
        dmesgln("Console: no serial input");
    return {};
}

void queue_console_input(u8 byte)
{
    if (s_console)
        s_console->queue_input(byte);
}

TTY& console_tty()
{
    return *s_console;
}

}
