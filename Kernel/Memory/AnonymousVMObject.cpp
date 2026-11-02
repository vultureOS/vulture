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

AnonymousVMObject::AnonymousVMObject(u64* entries, size_t page_count)
    : m_entries(entries)
    , m_page_count(page_count)
{
}

ErrorOr<NonnullRefPtr<AnonymousVMObject>> AnonymousVMObject::create(size_t page_count)
{
    size_t bytes;
    if (__builtin_mul_overflow(page_count, sizeof(u64), &bytes))
        return Error::from_errno(ENOMEM);
    auto* entries = static_cast<u64*>(kzalloc(bytes));
    if (!entries)
        return Error::from_errno(ENOMEM);
    auto* object = new (Base::nothrow) AnonymousVMObject(entries, page_count);
    if (!object) {
        kfree(entries);
        return Error::from_errno(ENOMEM);
    }
    return adopt_ref(*object);
}

AnonymousVMObject::~AnonymousVMObject()
{
    for (size_t i = 0; i < m_page_count; ++i)
        release_entry(m_entries[i]);
    kfree(m_entries);
}

void AnonymousVMObject::release_entry(u64 entry)
{
    if (entry & compressed_bit)
        Compressor::unref(reinterpret_cast<Compressor::Handle>(entry & ~compressed_bit));
    else if (entry)
        physical_memory().unref_user_page(PhysicalAddress(entry & ~incompressible_bit));
}

PhysicalAddress AnonymousVMObject::page(size_t index) const
{
    VERIFY(index < m_page_count);
    auto const entry = m_entries[index];
    return (entry & compressed_bit) ? PhysicalAddress() : PhysicalAddress(entry & ~incompressible_bit);
}

bool AnonymousVMObject::is_known_incompressible(size_t index) const
{
    VERIFY(index < m_page_count);
    return m_entries[index] & incompressible_bit;
}

void AnonymousVMObject::set_known_incompressible(size_t index, bool known)
{
    VERIFY(index < m_page_count);
    if (known)
        m_entries[index] |= incompressible_bit;
    else
        m_entries[index] &= ~incompressible_bit;
}

Compressor::Handle AnonymousVMObject::compressed_page(size_t index) const
{
    VERIFY(index < m_page_count);
    auto const entry = m_entries[index];
    return (entry & compressed_bit) ? reinterpret_cast<Compressor::Handle>(entry & ~compressed_bit) : nullptr;
}

void AnonymousVMObject::set_page(size_t index, PhysicalAddress page)
{
    VERIFY(index < m_page_count);
    auto const old = m_entries[index];
    m_entries[index] = page.get();
    release_entry(old);
}

void AnonymousVMObject::set_compressed_page(size_t index, Compressor::Handle handle)
{
    VERIFY(index < m_page_count);
    auto const old = m_entries[index];
    m_entries[index] = reinterpret_cast<u64>(handle) | compressed_bit;
    release_entry(old);
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
        auto const entry = m_entries[first + i];
        if (entry & compressed_bit)
            Compressor::ref(reinterpret_cast<Compressor::Handle>(entry & ~compressed_bit));
        else if (entry)
            physical_memory().ref_user_page(PhysicalAddress(entry & ~incompressible_bit));
        clone->m_entries[i] = entry;
    }
    return clone;
}

}
