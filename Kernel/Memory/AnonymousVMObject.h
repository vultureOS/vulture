/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Memory/PhysicalAddress.h>
#include <LibBase/Error.h>
#include <LibBase/RefPtr.h>

namespace Kernel {

// The pages behind a region of anonymous (not file-backed) user memory.
// A page is allocated the first time it is touched, filled with zeros.
// Pages are reference-counted (PhysicalMemoryManager), so after fork two
// objects may share a page until one of them writes to it.
class AnonymousVMObject final : public RefCounted<AnonymousVMObject> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<AnonymousVMObject>> create(size_t page_count);
    ~AnonymousVMObject();

    [[nodiscard]] size_t page_count() const { return m_page_count; }

    // A null address means "not allocated yet".
    [[nodiscard]] PhysicalAddress page(size_t index) const;
    // Stores `page`, taking over one reference to it; drops the old page's.
    void set_page(size_t index, PhysicalAddress page);
    // Drops the page's reference and forgets it.
    void release_page(size_t index);

    // A new object sharing pages [first, first + count) with this one.
    [[nodiscard]] ErrorOr<NonnullRefPtr<AnonymousVMObject>> clone_range(size_t first, size_t count) const;

private:
    AnonymousVMObject(PhysicalAddress* pages, size_t page_count);

    PhysicalAddress* m_pages { nullptr };
    size_t m_page_count { 0 };
};

}
