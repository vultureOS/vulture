/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Checksum.h>
#include <LibDatabase/Pager.h>
#include <LibDatabase/Platform.h>

namespace Database {

namespace {

constexpr char magic[16] = "vultureDatabase";
constexpr u32 format_version = 1;

// The header, at the start of page 1.
constexpr size_t header_size = 64;
constexpr size_t offset_version = 16;
constexpr size_t offset_page_size = 20;
constexpr size_t offset_page_count = 24;
constexpr size_t offset_free_head = 28;
constexpr size_t offset_free_count = 32;
constexpr size_t offset_schema_root = 36;
constexpr size_t offset_change_counter = 40;
constexpr size_t offset_checksum = 48;

// The journal: a header, then whole pages, each with its number and a checksum over both of them and the salt, which
// differs each time so that what's left of an older journal can't pass for this one's.
constexpr char journal_magic[8] = "vjournl";
constexpr size_t journal_header_size = 32;
constexpr size_t journal_entry_size = 8 + page_size;

template<typename T>
T read_at(u8 const* bytes, size_t offset)
{
    T value;
    __builtin_memcpy(&value, bytes + offset, sizeof(T));
    return value;
}

template<typename T>
void write_at(u8* bytes, size_t offset, T value)
{
    __builtin_memcpy(bytes + offset, &value, sizeof(T));
}

u32 entry_checksum(u64 salt, u32 number, u8 const* data)
{
    CRC32C crc;
    crc.update(ReadonlyBytes(reinterpret_cast<u8 const*>(&salt), sizeof(salt)));
    crc.update(ReadonlyBytes(reinterpret_cast<u8 const*>(&number), sizeof(number)));
    crc.update(ReadonlyBytes(data, page_size));
    return crc.digest();
}

u64 make_salt(u64 previous)
{
    // Only has to differ from the last one.
    return (previous + 0x9e37'79b9'7f4a'7c15ull) ^ (static_cast<u64>(getpid()) << 32);
}

ErrorOr<void> read_exactly(int fd, u8* buffer, size_t count, u64 offset)
{
    size_t done = 0;
    while (done < count) {
        long got = pread(fd, buffer + done, count - done, static_cast<long>(offset + done));
        if (got < 0) {
            if (Platform::last_error() == Platform::error_interrupted)
                continue;
            return Error::from_errno(Platform::last_error());
        }
        if (got == 0)
            return Error::from_string_literal("database: the file ends too soon");
        done += static_cast<size_t>(got);
    }
    return {};
}

}

Pager::Pager(int fd, int journal_fd)
    : m_fd(fd)
    , m_journal_fd(journal_fd)
{
}

Pager::~Pager()
{
    m_cache.for_each([](u32, CachedPage* page) { delete page; });
    for (auto* page : m_spare)
        delete page;
    if (m_fd >= 0)
        close(m_fd);
    if (m_journal_fd >= 0)
        close(m_journal_fd);
}

ErrorOr<NonnullOwnPtr<Pager>> Pager::open(StringView path)
{
    auto terminated = TRY(String::from_view(path));
    int fd = ::open(terminated.characters(), Platform::open_read_write_create, 0644);
    if (fd < 0)
        return Error::from_errno(Platform::last_error());
    StringBuilder journal_path;
    journal_path.append(path);
    journal_path.append("-journal"_sv);
    auto journal_terminated = TRY(journal_path.to_string());
    int journal_fd = ::open(journal_terminated.characters(), Platform::open_read_write_create, 0644);
    if (journal_fd < 0) {
        int saved = Platform::last_error();
        close(fd);
        return Error::from_errno(saved);
    }
    auto* raw = new (Base::nothrow) Pager(fd, journal_fd);
    if (!raw) {
        close(fd);
        close(journal_fd);
        return Error::from_errno(Platform::error_no_memory);
    }
    auto pager = adopt_own(*raw);
    TRY(pager->recover());
    TRY(pager->read_header());
    return pager;
}

void Pager::write_header_to(u8* bytes) const
{
    __builtin_memset(bytes, 0, header_size);
    __builtin_memcpy(bytes, magic, sizeof(magic));
    write_at<u32>(bytes, offset_version, format_version);
    write_at<u32>(bytes, offset_page_size, static_cast<u32>(page_size));
    write_at<u32>(bytes, offset_page_count, m_page_count);
    write_at<u32>(bytes, offset_free_head, m_free_head);
    write_at<u32>(bytes, offset_free_count, m_free_count);
    write_at<u32>(bytes, offset_schema_root, m_schema_root);
    write_at<u64>(bytes, offset_change_counter, m_change_counter);
    write_at<u32>(bytes, offset_checksum, crc32c(ReadonlyBytes(bytes, offset_checksum)));
}

ErrorOr<void> Pager::read_header()
{
    long const size = lseek(m_fd, 0, Platform::seek_end);
    if (size < 0)
        return Error::from_errno(Platform::last_error());
    if (size == 0) {
        // A new one: page 1 is the header's, and the catalogue is the database's to make.
        m_page_count = 1;
        auto* page = TRY(slot_for(1));
        __builtin_memset(page->data, 0, page_size);
        write_header_to(page->data);
        TRY(write_all(m_fd, page->data, page_size, 0));
        if (fsync(m_fd) != 0)
            return Error::from_errno(Platform::last_error());
        return {};
    }
    u8 bytes[header_size];
    TRY(read_exactly(m_fd, bytes, sizeof(bytes), 0));
    if (__builtin_memcmp(bytes, magic, sizeof(magic)) != 0)
        return Error::from_string_literal("database: this isn't a database");
    if (read_at<u32>(bytes, offset_version) != format_version || read_at<u32>(bytes, offset_page_size) != page_size)
        return Error::from_string_literal("database: made by a version that can't be read");
    if (read_at<u32>(bytes, offset_checksum) != crc32c(ReadonlyBytes(bytes, offset_checksum)))
        return Error::from_string_literal("database: the header is damaged");
    m_page_count = read_at<u32>(bytes, offset_page_count);
    m_free_head = read_at<u32>(bytes, offset_free_head);
    m_free_count = read_at<u32>(bytes, offset_free_count);
    m_schema_root = read_at<u32>(bytes, offset_schema_root);
    m_change_counter = read_at<u64>(bytes, offset_change_counter);
    return {};
}

ErrorOr<void> Pager::restore_from_journal()
{
    long const journal_length = lseek(m_journal_fd, 0, Platform::seek_end);
    if (journal_length < 0)
        return Error::from_errno(Platform::last_error());
    if (static_cast<size_t>(journal_length) < journal_header_size)
        return {};
    u8 header[journal_header_size];
    TRY(read_exactly(m_journal_fd, header, sizeof(header), 0));
    if (__builtin_memcmp(header, journal_magic, sizeof(journal_magic)) != 0)
        return {};
    u64 salt = read_at<u64>(header, 8);
    u32 pages_before = read_at<u32>(header, 16);
    if (read_at<u32>(header, 20) != crc32c(ReadonlyBytes(header, 20)))
        return {};

    auto buffer = TRY(try_make<CachedPage>());
    u64 offset = journal_header_size;
    bool restored = false;
    while (offset + journal_entry_size <= static_cast<u64>(journal_length)) {
        u8 entry_header[8];
        TRY(read_exactly(m_journal_fd, entry_header, sizeof(entry_header), offset));
        TRY(read_exactly(m_journal_fd, buffer->data, page_size, offset + 8));
        u32 number = read_at<u32>(entry_header, 0);
        // What wasn't finished being written wasn't flushed either, so no page in the file was changed on the strength of it.
        if (read_at<u32>(entry_header, 4) != entry_checksum(salt, number, buffer->data))
            break;
        TRY(write_all(m_fd, buffer->data, page_size, (static_cast<u64>(number) - 1) * page_size));
        restored = true;
        offset += journal_entry_size;
    }
    if (restored || pages_before > 0) {
        if (ftruncate(m_fd, static_cast<long>(static_cast<u64>(pages_before) * page_size)) != 0)
            return Error::from_errno(Platform::last_error());
        if (fsync(m_fd) != 0)
            return Error::from_errno(Platform::last_error());
    }
    return {};
}

ErrorOr<void> Pager::recover()
{
    TRY(restore_from_journal());
    if (ftruncate(m_journal_fd, 0) != 0)
        return Error::from_errno(Platform::last_error());
    if (fsync(m_journal_fd) != 0)
        return Error::from_errno(Platform::last_error());
    return {};
}

ErrorOr<void> Pager::write_all(int fd, u8 const* bytes, size_t count, u64 offset)
{
    if (m_writes_before_crash == 0)
        return Error::from_string_literal("database: (a test's crash)");
    if (m_writes_before_crash > 0 && --m_writes_before_crash == 0) {
        (void)pwrite(fd, bytes, count / 2, static_cast<long>(offset));
        return Error::from_string_literal("database: (a test's crash)");
    }
    size_t done = 0;
    while (done < count) {
        long wrote = pwrite(fd, bytes + done, count - done, static_cast<long>(offset + done));
        if (wrote < 0) {
            if (Platform::last_error() == Platform::error_interrupted)
                continue;
            return Error::from_errno(Platform::last_error());
        }
        done += static_cast<size_t>(wrote);
    }
    return {};
}

ErrorOr<void> Pager::evict_one()
{
    CachedPage* oldest = nullptr;
    m_cache.for_each([&](u32, CachedPage* page) {
        if (page->pins == 0 && (!oldest || page->last_used < oldest->last_used))
            oldest = page;
    });
    if (!oldest)
        return {}; // Everything's in use: there can be more than the limit for a moment.
    if (oldest->dirty) {
        TRY(sync_journal());
        TRY(write_page(*oldest));
    }
    u32 number = oldest->number;
    m_cache.remove(number);
    m_spare.append(oldest);
    --m_cached;
    return {};
}

ErrorOr<CachedPage*> Pager::slot_for(u32 number)
{
    // Evicting the oldest of thousands, one at a time, would be slow; a tenth of them at once isn't.
    if (m_cached >= m_cache_limit) {
        size_t const to_evict = max<size_t>(1, m_cache_limit / 10);
        for (size_t i = 0; i < to_evict; ++i)
            TRY(evict_one());
    }
    CachedPage* page;
    if (!m_spare.is_empty()) {
        page = m_spare.last();
        m_spare.remove(m_spare.size() - 1);
    } else {
        page = new (Base::nothrow) CachedPage;
        if (!page)
            return Error::from_errno(Platform::error_no_memory);
    }
    page->number = number;
    page->pins = 0;
    page->dirty = false;
    page->last_used = ++m_clock;
    TRY(m_cache.try_set(number, page));
    ++m_cached;
    return page;
}

ErrorOr<CachedPage*> Pager::load(u32 number)
{
    auto* page = TRY(slot_for(number));
    auto read = read_exactly(m_fd, page->data, page_size, (static_cast<u64>(number) - 1) * page_size);
    if (read.is_error()) {
        m_cache.remove(number);
        m_spare.append(page);
        --m_cached;
        return read.release_error();
    }
    return page;
}

ErrorOr<PageRef> Pager::get(u32 number)
{
    if (number == 0 || number > m_page_count)
        return Error::from_string_literal("database: a page that isn't in the file");
    if (auto** cached = m_cache.get(number)) {
        (*cached)->last_used = ++m_clock;
        return PageRef(*cached);
    }
    return PageRef(TRY(load(number)));
}

bool Pager::is_journaled(u32 number) const
{
    size_t const index = (number - 1) / 8;
    return index < m_journaled.size() && (m_journaled[index] >> ((number - 1) % 8)) & 1;
}

void Pager::mark_journaled(u32 number)
{
    size_t const index = (number - 1) / 8;
    while (m_journaled.size() <= index)
        m_journaled.append(0);
    m_journaled[index] = static_cast<u8>(m_journaled[index] | (1u << ((number - 1) % 8)));
}

ErrorOr<void> Pager::journal_page(u32 number, u8 const* data)
{
    if (m_journal_size == 0) {
        m_journal_salt = make_salt(m_journal_salt + m_change_counter);
        u8 header[journal_header_size] = {};
        __builtin_memcpy(header, journal_magic, sizeof(journal_magic));
        write_at<u64>(header, 8, m_journal_salt);
        write_at<u32>(header, 16, m_page_count_before);
        write_at<u32>(header, 20, crc32c(ReadonlyBytes(header, 20)));
        TRY(write_all(m_journal_fd, header, sizeof(header), 0));
        m_journal_size = journal_header_size;
    }
    u8 entry_header[8];
    write_at<u32>(entry_header, 0, number);
    write_at<u32>(entry_header, 4, entry_checksum(m_journal_salt, number, data));
    TRY(write_all(m_journal_fd, entry_header, sizeof(entry_header), m_journal_size));
    TRY(write_all(m_journal_fd, data, page_size, m_journal_size + sizeof(entry_header)));
    m_journal_size += journal_entry_size;
    m_journal_needs_sync = true;
    return {};
}

ErrorOr<void> Pager::sync_journal()
{
    if (!m_journal_needs_sync)
        return {};
    if (fsync(m_journal_fd) != 0)
        return Error::from_errno(Platform::last_error());
    m_journal_needs_sync = false;
    return {};
}

ErrorOr<void> Pager::write_page(CachedPage& page)
{
    TRY(write_all(m_fd, page.data, page_size, (static_cast<u64>(page.number) - 1) * page_size));
    page.dirty = false;
    return {};
}

ErrorOr<void> Pager::make_writable(PageRef const& reference)
{
    VERIFY(m_in_transaction);
    auto& page = *reference.page();
    // A page beyond where the file ended has nothing to put back.
    if (page.number <= m_page_count_before && !is_journaled(page.number)) {
        TRY(journal_page(page.number, page.data));
        mark_journaled(page.number);
    }
    page.dirty = true;
    return {};
}

ErrorOr<PageRef> Pager::allocate()
{
    VERIFY(m_in_transaction);
    if (m_free_head != 0) {
        auto reference = TRY(get(m_free_head));
        TRY(make_writable(reference));
        m_free_head = read_at<u32>(reference.data(), 0);
        --m_free_count;
        __builtin_memset(reference.data(), 0, page_size);
        return reference;
    }
    u32 number = ++m_page_count;
    auto* page = TRY(slot_for(number));
    __builtin_memset(page->data, 0, page_size);
    page->dirty = true;
    return PageRef(page);
}

ErrorOr<void> Pager::free(u32 number)
{
    VERIFY(m_in_transaction);
    auto reference = TRY(get(number));
    TRY(make_writable(reference));
    __builtin_memset(reference.data(), 0, page_size);
    write_at<u32>(reference.data(), 0, m_free_head);
    m_free_head = number;
    ++m_free_count;
    return {};
}

ErrorOr<void> Pager::begin()
{
    VERIFY(!m_in_transaction);
    m_in_transaction = true;
    m_page_count_before = m_page_count;
    m_journal_size = 0;
    m_journal_needs_sync = false;
    m_journaled.clear();
    return {};
}

ErrorOr<void> Pager::commit()
{
    VERIFY(m_in_transaction);
    bool any_dirty = false;
    m_cache.for_each([&](u32, CachedPage* page) {
        if (page->dirty)
            any_dirty = true;
    });
    if (!any_dirty && m_journal_size == 0 && m_page_count == m_page_count_before) {
        m_in_transaction = false;
        return {};
    }

    // Page 1, with what's in the header now.
    ++m_change_counter;
    auto first = TRY(get(1));
    TRY(make_writable(first));
    write_header_to(first.data());

    // First, everything needed to undo it; then it; and then it's done.
    TRY(sync_journal());
    Vector<CachedPage*> dirty;
    m_cache.for_each([&](u32, CachedPage* page) {
        if (page->dirty)
            dirty.append(page);
    });
    for (size_t i = 1; i < dirty.size(); ++i) {
        for (size_t j = i; j > 0 && dirty[j]->number < dirty[j - 1]->number; --j) {
            auto* moved = dirty[j];
            dirty[j] = dirty[j - 1];
            dirty[j - 1] = moved;
        }
    }
    for (auto* page : dirty)
        TRY(write_page(*page));
    if (fsync(m_fd) != 0)
        return Error::from_errno(Platform::last_error());
    if (m_writes_before_crash == 0)
        return Error::from_string_literal("database: (a test's crash)");
    if (ftruncate(m_journal_fd, 0) != 0)
        return Error::from_errno(Platform::last_error());
    if (fsync(m_journal_fd) != 0)
        return Error::from_errno(Platform::last_error());
    m_journal_size = 0;
    m_journaled.clear();
    m_in_transaction = false;
    return {};
}

void Pager::forget_cache()
{
    Vector<u32> numbers;
    m_cache.for_each([&](u32 number, CachedPage* page) {
        VERIFY(page->pins == 0);
        m_spare.append(page);
        numbers.append(number);
    });
    for (auto number : numbers)
        m_cache.remove(number);
    m_cached = 0;
}

ErrorOr<void> Pager::rollback()
{
    VERIFY(m_in_transaction);
    forget_cache();
    TRY(sync_journal());
    TRY(restore_from_journal());
    if (ftruncate(m_journal_fd, 0) != 0)
        return Error::from_errno(Platform::last_error());
    m_journal_size = 0;
    m_journaled.clear();
    m_in_transaction = false;
    // A database that never had anything committed has an empty header to go back to.
    TRY(read_header());
    return {};
}

}
