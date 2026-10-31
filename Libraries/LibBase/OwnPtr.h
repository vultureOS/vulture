/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/New.h>
#include <LibBase/StdLibExtras.h>

// Sole ownership: the object is deleted with its owner. Move-only.
//
//     auto buffer = TRY(try_make<Buffer>(size));  // NonnullOwnPtr<Buffer>

namespace Base {

// Never null, except after being moved from (using it then is a bug).
template<typename T>
class [[nodiscard]] NonnullOwnPtr {
public:
    enum class AdoptTag {
        Adopt,
    };

    NonnullOwnPtr(AdoptTag, T& object)
        : m_pointer(&object)
    {
    }

    template<typename U>
    requires(IsConvertible<U*, T*>)
    NonnullOwnPtr(NonnullOwnPtr<U>&& other)
        : m_pointer(other.leak_ptr())
    {
    }

    NonnullOwnPtr(NonnullOwnPtr&& other)
        : m_pointer(other.leak_ptr())
    {
    }

    NonnullOwnPtr(NonnullOwnPtr const&) = delete;
    NonnullOwnPtr& operator=(NonnullOwnPtr const&) = delete;

    NonnullOwnPtr& operator=(NonnullOwnPtr&& other)
    {
        NonnullOwnPtr moved(move(other));
        swap(m_pointer, moved.m_pointer);
        return *this;
    }

    ~NonnullOwnPtr() { delete m_pointer; }

    [[nodiscard]] T* ptr() const
    {
        VERIFY(m_pointer);
        return m_pointer;
    }

    T* operator->() const { return ptr(); }
    T& operator*() const { return *ptr(); }

    // Gives up ownership without deleting.
    [[nodiscard]] T* leak_ptr() { return exchange(m_pointer, nullptr); }

private:
    T* m_pointer { nullptr };
};

template<typename T>
NonnullOwnPtr<T> adopt_own(T& object)
{
    return NonnullOwnPtr<T>(NonnullOwnPtr<T>::AdoptTag::Adopt, object);
}

template<typename T>
class OwnPtr {
public:
    OwnPtr() = default;
    OwnPtr(nullptr_t) { }

    template<typename U>
    requires(IsConvertible<U*, T*>)
    OwnPtr(NonnullOwnPtr<U>&& other)
        : m_pointer(other.leak_ptr())
    {
    }

    OwnPtr(OwnPtr&& other)
        : m_pointer(other.leak_ptr())
    {
    }

    OwnPtr(OwnPtr const&) = delete;
    OwnPtr& operator=(OwnPtr const&) = delete;

    OwnPtr& operator=(OwnPtr&& other)
    {
        OwnPtr moved(move(other));
        swap(m_pointer, moved.m_pointer);
        return *this;
    }

    ~OwnPtr() { clear(); }

    void clear() { delete exchange(m_pointer, nullptr); }

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

    [[nodiscard]] T* leak_ptr() { return exchange(m_pointer, nullptr); }

    [[nodiscard]] NonnullOwnPtr<T> release_nonnull()
    {
        VERIFY(m_pointer);
        return adopt_own(*leak_ptr());
    }

private:
    T* m_pointer { nullptr };
};

// Creates an object, or fails when memory runs out.
template<typename T, typename... Args>
ErrorOr<NonnullOwnPtr<T>> try_make(Args&&... arguments)
{
    auto* object = new (nothrow) T(forward<Args>(arguments)...);
    if (!object)
        return Error::from_string_literal("out of memory");
    return adopt_own(*object);
}

}

using Base::adopt_own;
using Base::NonnullOwnPtr;
using Base::OwnPtr;
using Base::try_make;
