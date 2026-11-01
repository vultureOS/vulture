/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/New.h>
#include <LibBase/Span.h>
#include <LibBase/StdLibExtras.h>

// A growable array. Growing can fail when memory runs out: the try_*
// functions report that as an error; the others treat it as a bug.

namespace Base {

template<typename T>
class Vector {
public:
    Vector() = default;

    Vector(Vector&& other)
        : m_data(exchange(other.m_data, nullptr))
        , m_size(exchange(other.m_size, 0))
        , m_capacity(exchange(other.m_capacity, 0))
    {
    }

    Vector(Vector const& other)
    {
        MUST(try_ensure_capacity(other.m_size));
        for (auto const& element : other)
            unchecked_append(element);
    }

    ~Vector() { clear(); }

    Vector& operator=(Vector&& other)
    {
        if (this != &other) {
            clear();
            m_data = exchange(other.m_data, nullptr);
            m_size = exchange(other.m_size, 0);
            m_capacity = exchange(other.m_capacity, 0);
        }
        return *this;
    }

    Vector& operator=(Vector const& other)
    {
        if (this != &other) {
            Vector copy(other);
            *this = move(copy);
        }
        return *this;
    }

    [[nodiscard]] size_t size() const { return m_size; }
    [[nodiscard]] size_t capacity() const { return m_capacity; }
    [[nodiscard]] bool is_empty() const { return m_size == 0; }
    [[nodiscard]] T* data() { return m_data; }
    [[nodiscard]] T const* data() const { return m_data; }

    T& operator[](size_t index)
    {
        VERIFY(index < m_size);
        return m_data[index];
    }

    T const& operator[](size_t index) const
    {
        VERIFY(index < m_size);
        return m_data[index];
    }

    T& first() { return (*this)[0]; }
    T const& first() const { return (*this)[0]; }
    T& last() { return (*this)[m_size - 1]; }
    T const& last() const { return (*this)[m_size - 1]; }

    [[nodiscard]] Span<T> span() { return { m_data, m_size }; }
    [[nodiscard]] Span<T const> span() const { return { m_data, m_size }; }

    T* begin() { return m_data; }
    T* end() { return m_data + m_size; }
    T const* begin() const { return m_data; }
    T const* end() const { return m_data + m_size; }

    ErrorOr<void> try_ensure_capacity(size_t needed)
    {
        if (needed <= m_capacity)
            return {};
        size_t new_capacity = max<size_t>(needed, max<size_t>(4, m_capacity + m_capacity / 2));
        size_t bytes;
        if (__builtin_mul_overflow(new_capacity, sizeof(T), &bytes))
            return Error::from_errno(12);
        auto* new_data = static_cast<T*>(operator new(bytes, nothrow));
        if (!new_data)
            return Error::from_errno(12);
        for (size_t i = 0; i < m_size; ++i) {
            new (&new_data[i]) T(move(m_data[i]));
            m_data[i].~T();
        }
        operator delete(m_data);
        m_data = new_data;
        m_capacity = new_capacity;
        return {};
    }

    template<typename U = T>
    ErrorOr<void> try_append(U&& value)
    {
        if (m_size == m_capacity) {
            // `value` may be one of our own elements: keep it through the move.
            T kept(forward<U>(value));
            TRY(try_ensure_capacity(m_size + 1));
            unchecked_append(move(kept));
            return {};
        }
        unchecked_append(forward<U>(value));
        return {};
    }

    template<typename U = T>
    void append(U&& value)
    {
        MUST(try_append(forward<U>(value)));
    }

    template<typename U = T>
    ErrorOr<void> try_insert(size_t index, U&& value)
    {
        VERIFY(index <= m_size);
        TRY(try_ensure_capacity(m_size + 1));
        if (index == m_size) {
            unchecked_append(forward<U>(value));
            return {};
        }
        new (&m_data[m_size]) T(move(m_data[m_size - 1]));
        for (size_t i = m_size - 1; i > index; --i)
            m_data[i] = move(m_data[i - 1]);
        m_data[index] = T(forward<U>(value));
        ++m_size;
        return {};
    }

    template<typename U = T>
    void insert(size_t index, U&& value)
    {
        MUST(try_insert(index, forward<U>(value)));
    }

    void remove(size_t index)
    {
        VERIFY(index < m_size);
        for (size_t i = index; i + 1 < m_size; ++i)
            m_data[i] = move(m_data[i + 1]);
        m_data[--m_size].~T();
    }

    template<typename Predicate>
    size_t remove_all_matching(Predicate predicate)
    {
        size_t kept = 0;
        for (size_t i = 0; i < m_size; ++i) {
            if (!predicate(m_data[i])) {
                if (kept != i)
                    m_data[kept] = move(m_data[i]);
                ++kept;
            }
        }
        size_t removed = m_size - kept;
        while (m_size > kept)
            m_data[--m_size].~T();
        return removed;
    }

    T take_last()
    {
        VERIFY(m_size > 0);
        T value = move(m_data[m_size - 1]);
        m_data[--m_size].~T();
        return value;
    }

    // Grows (with default-constructed elements) or shrinks to `size`.
    ErrorOr<void> try_resize(size_t size)
    {
        TRY(try_ensure_capacity(size));
        while (m_size < size)
            new (&m_data[m_size++]) T();
        while (m_size > size)
            m_data[--m_size].~T();
        return {};
    }

    void clear()
    {
        while (m_size > 0)
            m_data[--m_size].~T();
        operator delete(m_data);
        m_data = nullptr;
        m_capacity = 0;
    }

    template<typename U>
    [[nodiscard]] bool contains_slow(U const& value) const
    {
        for (auto const& element : *this) {
            if (element == value)
                return true;
        }
        return false;
    }

private:
    template<typename U>
    void unchecked_append(U&& value)
    {
        new (&m_data[m_size++]) T(forward<U>(value));
    }

    T* m_data { nullptr };
    size_t m_size { 0 };
    size_t m_capacity { 0 };
};

}

using Base::Vector;
