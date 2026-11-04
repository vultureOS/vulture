/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/FileSystem/DevFS.h>
#include <Kernel/FileSystem/FSEvents.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Security/Credentials.h>
#include <Kernel/Security/Sandbox.h>
#include <Kernel/Tasks/Process.h>
#include <LibBase/Atomic.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

namespace Kernel::FSEvents {

namespace {

struct Event {
    u64 id { 0 };
    u32 flags { 0 };
    String path;
};

// The latest few, so that someone keeping up needn't go to the disk for them.
constexpr size_t kept_in_memory = 4096;

Mutex s_lock;
WaitQueue s_waiters;
// In a circle once it's full, with the oldest at s_recent_start.
Vector<Event> s_recent;
size_t s_recent_start { 0 };
FileSystem* s_volume { nullptr };
Atomic<u64> s_next_id { 1 };

// What's kept on a volume, and a temporary one with nothing to keep it in, are told apart here: if the volume
// can't, ids still go up.
u64 next_id_for(FileSystem& file_system, u32 flags, StringView path)
{
    auto kept = file_system.append_event(flags, path);
    if (!kept.is_error()) {
        s_next_id.store(kept.value() + 1);
        return kept.value();
    }
    return s_next_id.fetch_add(1);
}

bool is_within(StringView path, StringView watched)
{
    if (watched == "/"_sv)
        return true;
    if (!path.starts_with(watched))
        return false;
    return path.length() == watched.length() || path[watched.length()] == '/';
}

class Stream final : public Device {
public:
    Stream()
        : Device(InodeType::CharacterDevice, 0444, device_number(6, 1))
        , m_next(s_next_id.load())
    {
    }

    [[nodiscard]] bool can_read() const override
    {
        return m_next < s_next_id.load() || m_owed_history_done || m_owed_scans;
    }

    ErrorOr<size_t> read(u64, size_t count, IOBuffer const& buffer) override
    {
        if (count < sizeof(vulture_fsevent) + 2)
            return Error::from_errno(EINVAL);
        constexpr size_t most_at_once = 64 * KiB;
        size_t const room = min(count, most_at_once);
        auto* bytes = static_cast<u8*>(kmalloc(room));
        if (!bytes)
            return Error::from_errno(ENOMEM);
        size_t filled = 0;
        auto const finish = [&]() -> ErrorOr<size_t> {
            auto copied = buffer.write(0, { bytes, filled });
            kfree(bytes);
            TRY(copied);
            return filled;
        };
        for (;;) {
            filled = collect(bytes, room);
            if (filled)
                return finish();
            // (Everything so far has been for paths nobody asked about, or it's someone else's business.)
            if (!s_waiters.wait_until_interruptible([this] { return can_read(); })) {
                kfree(bytes);
                return Error::from_errno(EINTR);
            }
        }
    }

    ErrorOr<FlatPtr> ioctl(FileDescription&, unsigned request, FlatPtr argument) override
    {
        if (request == FSEVENTS_LATEST)
            return copy_to_user(argument, s_next_id.load() - 1).is_error() ? Error::from_errno(EFAULT) : ErrorOr<FlatPtr>(0);
        if (request != FSEVENTS_WATCH)
            return Error::from_errno(ENOTTY);
        auto const watch = TRY(copy_from_user<vulture_fsevents_watch>(argument));
        if (watch.path_count > 1024)
            return Error::from_errno(EINVAL);
        Vector<String> paths;
        auto* process = Process::current();
        for (unsigned i = 0; i < watch.path_count; ++i) {
            auto const pointer = TRY(copy_from_user<FlatPtr>(reinterpret_cast<FlatPtr>(watch.paths) + i * sizeof(FlatPtr)));
            auto path = TRY(process->copy_path_from_user(pointer));
            // Where it really is: that's what changes will say.
            auto resolved = VirtualFileSystem::resolve(path.view(), process->current_directory().view());
            TRY(paths.try_append(resolved.is_error() ? move(path) : move(resolved.value().path)));
        }
        MutexLocker locker(s_lock);
        m_paths = move(paths);
        u64 const latest = s_next_id.load();
        if (watch.since == FSEVENTS_SINCE_NOW) {
            m_next = latest;
            m_owed_history_done = false;
            m_owed_scans = false;
        } else {
            m_next = watch.since + 1;
            m_owed_history_done = true;
            u64 const oldest = oldest_available();
            // Too long ago to say what happened: what they knew about is out of date, and that's all that can be said.
            m_owed_scans = m_next < oldest && watch.since != 0;
            if (m_next < oldest)
                m_next = oldest;
            m_history_end = latest;
        }
        notify_pollers();
        return 0;
    }

private:
    [[nodiscard]] u64 oldest_available() const
    {
        if (s_volume) {
            auto const oldest = s_volume->oldest_event();
            if (oldest != 0)
                return oldest;
        }
        return s_recent.is_empty() ? s_next_id.load() : s_recent[s_recent_start].id;
    }

    [[nodiscard]] bool may_hear_of(StringView path) const
    {
        bool wanted = m_paths.is_empty();
        for (size_t i = 0; i < m_paths.size() && !wanted; ++i)
            wanted = is_within(path, m_paths[i].view());
        if (!wanted)
            return false;
        // That something exists is itself something to be allowed to know.
        if (sandbox_check(SandboxOperation::FileRead, path).is_error())
            return false;
        auto const* credentials = current_credentials();
        if (!credentials || credentials->is_superuser())
            return true;
        auto const parent = VirtualFileSystem::resolve_parent(path, "/"_sv);
        if (parent.is_error())
            return parent.error().code() != EACCES;
        constexpr int search_access = 1;
        return !check_access(parent.value().directory->metadata(), search_access).is_error();
    }

    bool add(u8* bytes, size_t room, size_t& filled, u64 id, u32 flags, StringView path)
    {
        size_t const length = (sizeof(vulture_fsevent) + path.length() + 1 + 7) & ~static_cast<size_t>(7);
        if (filled + length > room)
            return false;
        vulture_fsevent header {};
        header.id = id;
        header.flags = flags;
        header.path_length = static_cast<unsigned int>(path.length() + 1);
        __builtin_memset(bytes + filled, 0, length);
        __builtin_memcpy(bytes + filled, &header, sizeof(header));
        __builtin_memcpy(bytes + filled + sizeof(header), path.characters_without_null_termination(), path.length());
        filled += length;
        return true;
    }

    // As much as fits, of what's next.
    size_t collect(u8* bytes, size_t room)
    {
        MutexLocker locker(s_lock);
        size_t filled = 0;
        if (m_owed_scans) {
            bool const none = m_paths.is_empty();
            for (size_t i = 0; i < (none ? 1 : m_paths.size()); ++i)
                add(bytes, room, filled, m_next, FSE_MUST_SCAN_SUBDIRECTORIES, none ? "/"_sv : m_paths[i].view());
            m_owed_scans = false;
        }
        u64 const end = s_next_id.load();
        // Older than what's in memory: from the volume, which also has what hasn't been committed yet.
        u64 const oldest_here = s_recent.is_empty() ? end : s_recent[s_recent_start].id;
        if (m_next < oldest_here && s_volume) {
            // Only copied out while the volume is busy with this: asking who may know of a path goes back to it.
            Vector<Event> older;
            size_t size = 0;
            auto const visited = s_volume->for_each_event(m_next, [&](u64 id, u32 flags, StringView path) {
                if (id >= oldest_here)
                    return false;
                auto copy = String::from_view(path);
                if (copy.is_error() || older.try_append(Event { id, flags, copy.release_value() }).is_error())
                    return false;
                size += sizeof(vulture_fsevent) + path.length() + 8;
                return size < room;
            });
            bool const all = !visited.is_error() && size < room;
            for (auto const& event : older) {
                if (may_hear_of(event.path.view()) && !add(bytes, room, filled, event.id, event.flags, event.path.view()))
                    return filled;
                m_next = event.id + 1;
            }
            if (!all)
                return filled;
            if (m_next < oldest_here)
                m_next = oldest_here;
        } else if (m_next < oldest_here) {
            m_next = oldest_here;
        }
        for (size_t i = 0; i < s_recent.size(); ++i) {
            auto const& event = s_recent[(s_recent_start + i) % s_recent.size()];
            if (event.id < m_next)
                continue;
            if (may_hear_of(event.path.view()) && !add(bytes, room, filled, event.id, event.flags, event.path.view()))
                return filled;
            m_next = event.id + 1;
        }
        if (m_next < end)
            m_next = end;
        if (m_owed_history_done && m_next >= m_history_end) {
            if (add(bytes, room, filled, m_history_end - 1, FSE_HISTORY_DONE, "/"_sv))
                m_owed_history_done = false;
        }
        return filled;
    }

    u64 m_next;
    u64 m_history_end { 0 };
    bool m_owed_history_done { false };
    bool m_owed_scans { false };
    Vector<String> m_paths;
};

class Control final : public Device {
public:
    Control()
        : Device(InodeType::CharacterDevice, 0444, device_number(6, 0))
    {
    }
    ErrorOr<NonnullRefPtr<Inode>> open_inode(int) override
    {
        auto* stream = new (Base::nothrow) Stream;
        if (!stream)
            return Error::from_errno(ENOMEM);
        return adopt_ref(*static_cast<Inode*>(stream));
    }
};

u32 type_flag(InodeType type)
{
    switch (type) {
    case InodeType::Directory:
        return FSE_ITEM_IS_DIRECTORY;
    case InodeType::SymbolicLink:
        return FSE_ITEM_IS_SYMBOLIC_LINK;
    default:
        return FSE_ITEM_IS_FILE;
    }
}

}

ErrorOr<void> create_device()
{
    auto* device = new (Base::nothrow) Control;
    if (!device)
        return Error::from_errno(ENOMEM);
    return DevFS::the().add_device("fsevents"_sv, adopt_ref(*static_cast<Inode*>(device)));
}

void use_volume(FileSystem& file_system)
{
    MutexLocker locker(s_lock);
    s_volume = &file_system;
    u64 const next = file_system.next_event();
    if (next > s_next_id.load())
        s_next_id.store(next);
}

void record(FileSystem& file_system, StringView path, u32 flags)
{
    // Only what's kept matters to anyone, and that's user data.
    if (&file_system != s_volume)
        return;
    auto copy = String::from_view(path);
    if (copy.is_error())
        return;
    {
        MutexLocker locker(s_lock);
        u64 const id = next_id_for(file_system, flags, path);
        Event event { id, flags, copy.release_value() };
        if (s_recent.size() < kept_in_memory) {
            (void)s_recent.try_append(move(event));
        } else {
            s_recent[s_recent_start] = move(event);
            s_recent_start = (s_recent_start + 1) % kept_in_memory;
        }
    }
    s_waiters.wake_all();
    notify_pollers();
}

void record(Inode const& inode, StringView path, u32 flags)
{
    record(inode.file_system(), path, flags | type_flag(inode.metadata().type));
}

void record_removed(FileSystem& file_system, InodeType type, StringView path)
{
    record(file_system, path, FSE_ITEM_REMOVED | type_flag(type));
}

}
