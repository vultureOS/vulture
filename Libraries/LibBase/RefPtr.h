/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Atomic.h>
#include <LibBase/Error.h>
#include <LibBase/New.h>
#include <LibBase/StdLibExtras.h>

// Shared ownership. A class derives from RefCounted<T>; the object starts
// with one reference, which adopt_ref() hands to a NonnullRefPtr. The count
// is atomic, so references may be taken and dropped on any processor. The
// object is deleted when the last reference goes.
//
//     auto thread = TRY(try_make_ref_counted<Thread>(...));  // NonnullRefPtr<Thread>
//     RefPtr<Thread> maybe = thread;                          // count is 2

namespace Base {

template<typename T>
class RefCounted {
public:
    RefCounted(RefCounted const&) = delete;
    RefCounted& operator=(RefCounted const&) = delete;

    void ref() const
    {
        u32 previous = m_ref_count.fetch_add(1, MemoryOrder::Relaxed);
        VERIFY(previous > 0);
    }

    // Returns true if this dropped the last reference (the object is gone).
    bool unref() const
    {
        // Release publishes this owner's writes; acquire (for the last
        // owner) sees everyone else's before destruction.
        u32 previous = m_ref_count.fetch_sub(1, MemoryOrder::AcquireRelease);
        VERIFY(previous > 0);
        if (previous != 1)
            return false;
        delete static_cast<T const*>(this);
        return true;
    }

    [[nodiscard]] u32 ref_count() const { return m_ref_count.load(MemoryOrder::Relaxed); }

protected:
    RefCounted() = default;
    ~RefCounted() { VERIFY(m_ref_count.load(MemoryOrder::Relaxed) == 0); }

private:
    mutable Atomic<u32> m_ref_count { 1 };
};

template<typename T>
class NonnullRefPtr;

template<typename T>
NonnullRefPtr<T> adopt_ref(T&);

// Never null, except after being moved from (using it then is a bug).
template<typename T>
class [[nodiscard]] NonnullRefPtr {
public:
    NonnullRefPtr(T& object)
        : m_pointer(&object)
    {
        m_pointer->ref();
    }

    template<typename U>
    requires(IsConvertible<U*, T*>)
    NonnullRefPtr(NonnullRefPtr<U> const& other)
        : m_pointer(other.ptr())
    {
        m_pointer->ref();
    }

    NonnullRefPtr(NonnullRefPtr const& other)
        : NonnullRefPtr(*other.ptr())
    {
    }

    template<typename U>
    requires(IsConvertible<U*, T*>)
    NonnullRefPtr(NonnullRefPtr<U>&& other)
        : m_pointer(other.leak_ref())
    {
    }

    NonnullRefPtr(NonnullRefPtr&& other)
        : m_pointer(other.leak_ref())
    {
    }

    ~NonnullRefPtr()
    {
        if (m_pointer)
            m_pointer->unref();
    }

    NonnullRefPtr& operator=(NonnullRefPtr other)
    {
        swap(m_pointer, other.m_pointer);
        return *this;
    }

    [[nodiscard]] T* ptr() const
    {
        VERIFY(m_pointer);
        return m_pointer;
    }

    T* operator->() const { return ptr(); }
    T& operator*() const { return *ptr(); }

    // Gives up this reference without dropping it.
    [[nodiscard]] T* leak_ref() { return exchange(m_pointer, nullptr); }

    bool operator==(NonnullRefPtr const& other) const { return m_pointer == other.m_pointer; }

private:
    enum class AdoptTag {
        Adopt,
    };

    NonnullRefPtr(AdoptTag, T& object)
        : m_pointer(&object)
    {
    }

    friend NonnullRefPtr adopt_ref<T>(T&);

    T* m_pointer { nullptr };
};

// Takes over the initial reference of a newly created object.
template<typename T>
NonnullRefPtr<T> adopt_ref(T& object)
{
    return NonnullRefPtr<T>(NonnullRefPtr<T>::AdoptTag::Adopt, object);
}

template<typename T>
class RefPtr {
public:
    RefPtr() = default;
    RefPtr(nullptr_t) { }

    RefPtr(T* pointer)
        : m_pointer(pointer)
    {
        if (m_pointer)
            m_pointer->ref();
    }

    RefPtr(T& object)
        : RefPtr(&object)
    {
    }

    template<typename U>
    requires(IsConvertible<U*, T*>)
    RefPtr(NonnullRefPtr<U> const& other)
        : RefPtr(other.ptr())
    {
    }

    template<typename U>
    requires(IsConvertible<U*, T*>)
    RefPtr(NonnullRefPtr<U>&& other)
        : m_pointer(other.leak_ref())
    {
    }

    RefPtr(RefPtr const& other)
        : RefPtr(other.m_pointer)
    {
    }

    RefPtr(RefPtr&& other)
        : m_pointer(other.leak_ref())
    {
    }

    ~RefPtr() { clear(); }

    RefPtr& operator=(RefPtr other)
    {
        swap(m_pointer, other.m_pointer);
        return *this;
    }

    void clear()
    {
        if (auto* pointer = exchange(m_pointer, nullptr))
            pointer->unref();
    }

    [[nodiscard]] T* ptr() const { return m_pointer; }
    [[nodiscard]] bool is_null() const { return !m_pointer; }
    explicit operator bool() const { return m_pointer; }

    T* operator->() const
    {
        VERIFY(m_pointer);
        return m_pointer;
    }

    T& operator*() const
    {
        VERIFY(m_pointer);
        return *m_pointer;
    }

    [[nodiscard]] T* leak_ref() { return exchange(m_pointer, nullptr); }

    [[nodiscard]] NonnullRefPtr<T> release_nonnull()
    {
        VERIFY(m_pointer);
        return adopt_ref(*leak_ref());
    }

    bool operator==(RefPtr const& other) const { return m_pointer == other.m_pointer; }
    bool operator==(T const* other) const { return m_pointer == other; }

private:
    T* m_pointer { nullptr };
};

// Creates a reference-counted object, or fails when memory runs out.
template<typename T, typename... Args>
ErrorOr<NonnullRefPtr<T>> try_make_ref_counted(Args&&... arguments)
{
    auto* object = new (nothrow) T(forward<Args>(arguments)...);
    if (!object)
        return Error::from_string_literal("out of memory");
    return adopt_ref(*object);
}

}

using Base::adopt_ref;
using Base::NonnullRefPtr;
using Base::RefCounted;
using Base::RefPtr;
using Base::try_make_ref_counted;
