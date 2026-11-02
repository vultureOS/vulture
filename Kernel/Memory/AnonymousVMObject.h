/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Memory/Compressor.h>
#include <Kernel/Memory/PhysicalAddress.h>
#include <LibBase/Error.h>
#include <LibBase/RefPtr.h>

namespace Kernel {

// The pages behind a region of anonymous (not file-backed) user memory.
// A page is allocated the first time it is touched, filled with zeros.
// Pages are reference-counted (PhysicalMemoryManager), so after fork two
// objects may share a page until one of them writes to it.
//
// A page may also be in the compressor's pool instead, in which case it has
// no address until it is got back (see Memory/Compressor.h).
class AnonymousVMObject final : public RefCounted<AnonymousVMObject> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<AnonymousVMObject>> create(size_t page_count);
    ~AnonymousVMObject();

    [[nodiscard]] size_t page_count() const { return m_page_count; }

    // A null address means "not allocated yet", or that it is compressed.
    [[nodiscard]] PhysicalAddress page(size_t index) const;
    // The page in the pool, or null.
    [[nodiscard]] Compressor::Handle compressed_page(size_t index) const;
    // Stores `page`, taking over one reference to it; drops the old page's.
    void set_page(size_t index, PhysicalAddress page);
    // Stores a page in the pool, taking over one reference to it; drops the old page's.
    void set_compressed_page(size_t index, Compressor::Handle);
    // It wouldn't compress. That stays true until it could have changed, which takes a page fault, as
    // it isn't mapped: so there's no need to try again until then.
    [[nodiscard]] bool is_known_incompressible(size_t index) const;
    void set_known_incompressible(size_t index, bool);
    // Drops the page's reference and forgets it.
    void release_page(size_t index);

    // A new object sharing pages [first, first + count) with this one.
    [[nodiscard]] ErrorOr<NonnullRefPtr<AnonymousVMObject>> clone_range(size_t first, size_t count) const;

private:
    AnonymousVMObject(u64* entries, size_t page_count);

    void release_entry(u64 entry);

    // A page's address; or, with this bit, a pointer to it in the pool; or 0.
    static constexpr u64 compressed_bit = 1;
    // On a page's address: see is_known_incompressible().
    static constexpr u64 incompressible_bit = 2;
    u64* m_entries { nullptr };
    size_t m_page_count { 0 };
};

}
