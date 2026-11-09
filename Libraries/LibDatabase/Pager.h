/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/HashMap.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

// A database file, as numbered pages, the first numbered 1. Changes wait in memory until a transaction commits. Before a
// page in the file is overwritten, what it held goes into a journal beside the file and is flushed there, so that a crash at
// any moment, found on the next open, can be undone; a transaction has happened at the moment the journal is emptied.
//
// One process has a database open at a time: it's what a service does, and others ask it.

namespace Database {

constexpr size_t page_size = 8192;

struct CachedPage {
    u32 number { 0 };
    u32 pins { 0 };
    u64 last_used { 0 };
    bool dirty { false };
    u8 data[page_size];
};

// Keeps a page in memory while it's held.
class PageRef {
public:
    PageRef() = default;
    explicit PageRef(CachedPage* page)
        : m_page(page)
    {
        if (m_page)
            ++m_page->pins;
    }
    PageRef(PageRef const& other)
        : PageRef(other.m_page)
    {
    }
    PageRef(PageRef&& other)
        : m_page(exchange(other.m_page, nullptr))
    {
    }
    ~PageRef() { release(); }
    PageRef& operator=(PageRef const& other)
    {
        if (this != &other) {
            release();
            m_page = other.m_page;
            if (m_page)
                ++m_page->pins;
        }
        return *this;
    }
    PageRef& operator=(PageRef&& other)
    {
        if (this != &other) {
            release();
            m_page = exchange(other.m_page, nullptr);
        }
        return *this;
    }

    [[nodiscard]] u8* data() const { return m_page->data; }
    [[nodiscard]] u32 number() const { return m_page->number; }
    [[nodiscard]] explicit operator bool() const { return m_page != nullptr; }
    [[nodiscard]] CachedPage* page() const { return m_page; }

private:
    void release()
    {
        if (m_page)
            --m_page->pins;
        m_page = nullptr;
    }

    CachedPage* m_page { nullptr };
};

class Pager {
public:
    static ErrorOr<NonnullOwnPtr<Pager>> open(StringView path);
    ~Pager();

    ErrorOr<PageRef> get(u32 number);
    // Before changing what's in a page.
    ErrorOr<void> make_writable(PageRef const&);
    // A page all zeroes, and already writable.
    ErrorOr<PageRef> allocate();
    ErrorOr<void> free(u32 number);

    ErrorOr<void> begin();
    ErrorOr<void> commit();
    ErrorOr<void> rollback();
    [[nodiscard]] bool in_transaction() const { return m_in_transaction; }

    [[nodiscard]] u32 schema_root() const { return m_schema_root; }
    void set_schema_root(u32 root) { m_schema_root = root; }
    [[nodiscard]] u32 page_count() const { return m_page_count; }
    [[nodiscard]] u32 free_page_count() const { return m_free_count; }
    // How many pages are held in memory, at most.
    void set_cache_pages(size_t count) { m_cache_limit = count; }

    // Tests: after this many more writes to any file, every write fails, the one that crosses the line being left half
    // done, as with the power going off.
    void crash_after_writes_for_testing(i64 writes) { m_writes_before_crash = writes; }

private:
    Pager(int fd, int journal_fd);

    ErrorOr<void> read_header();
    void write_header_to(u8*) const;
    ErrorOr<void> recover();
    ErrorOr<CachedPage*> load(u32 number);
    ErrorOr<CachedPage*> slot_for(u32 number);
    ErrorOr<void> evict_one();
    ErrorOr<void> journal_page(u32 number, u8 const*);
    ErrorOr<void> sync_journal();
    ErrorOr<void> write_page(CachedPage&);
    ErrorOr<void> write_all(int fd, u8 const*, size_t, u64 offset);
    ErrorOr<void> restore_from_journal();
    [[nodiscard]] bool is_journaled(u32 number) const;
    void mark_journaled(u32 number);
    void forget_cache();

    int m_fd { -1 };
    int m_journal_fd { -1 };

    HashMap<u32, CachedPage*> m_cache;
    Vector<CachedPage*> m_spare;
    size_t m_cache_limit { 4096 };
    size_t m_cached { 0 };
    u64 m_clock { 0 };

    u32 m_page_count { 0 };
    u32 m_free_head { 0 };
    u32 m_free_count { 0 };
    u32 m_schema_root { 0 };
    u64 m_change_counter { 0 };

    bool m_in_transaction { false };
    u32 m_page_count_before { 0 };
    u64 m_journal_salt { 0 };
    u64 m_journal_size { 0 };
    bool m_journal_needs_sync { false };
    Vector<u8> m_journaled;
    i64 m_writes_before_crash { -1 };
};

}
