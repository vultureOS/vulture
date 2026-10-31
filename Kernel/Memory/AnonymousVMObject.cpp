/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Memory/AnonymousVMObject.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/PhysicalMemory.h>

namespace Kernel {

AnonymousVMObject::AnonymousVMObject(PhysicalAddress* pages, size_t page_count)
    : m_pages(pages)
    , m_page_count(page_count)
{
}

ErrorOr<NonnullRefPtr<AnonymousVMObject>> AnonymousVMObject::create(size_t page_count)
{
    size_t bytes;
    if (__builtin_mul_overflow(page_count, sizeof(PhysicalAddress), &bytes))
        return Error::from_errno(ENOMEM);
    auto* pages = static_cast<PhysicalAddress*>(kzalloc(bytes));
    if (!pages)
        return Error::from_errno(ENOMEM);
    auto* object = new (Base::nothrow) AnonymousVMObject(pages, page_count);
    if (!object) {
        kfree(pages);
        return Error::from_errno(ENOMEM);
    }
    return adopt_ref(*object);
}

AnonymousVMObject::~AnonymousVMObject()
{
    for (size_t i = 0; i < m_page_count; ++i) {
        if (m_pages[i].get())
            physical_memory().unref_user_page(m_pages[i]);
    }
    kfree(m_pages);
}

PhysicalAddress AnonymousVMObject::page(size_t index) const
{
    VERIFY(index < m_page_count);
    return m_pages[index];
}

void AnonymousVMObject::set_page(size_t index, PhysicalAddress page)
{
    VERIFY(index < m_page_count);
    auto old = m_pages[index];
    m_pages[index] = page;
    if (old.get())
        physical_memory().unref_user_page(old);
}

void AnonymousVMObject::release_page(size_t index)
{
    set_page(index, PhysicalAddress());
}

ErrorOr<NonnullRefPtr<AnonymousVMObject>> AnonymousVMObject::clone_range(size_t first, size_t count) const
{
    VERIFY(first + count <= m_page_count);
    auto clone = TRY(create(count));
    for (size_t i = 0; i < count; ++i) {
        auto page = m_pages[first + i];
        if (page.get()) {
            physical_memory().ref_user_page(page);
            clone->m_pages[i] = page;
        }
    }
    return clone;
}

}
