/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/TypeTraits.h>
#include <LibBase/Types.h>

// Atomic operations on integers, enums and pointers, built on the compiler's
// __atomic builtins. Every operation names its memory order; the default is
// sequentially consistent.

namespace Base {

enum class MemoryOrder : int {
    Relaxed = __ATOMIC_RELAXED,
    Acquire = __ATOMIC_ACQUIRE,
    Release = __ATOMIC_RELEASE,
    AcquireRelease = __ATOMIC_ACQ_REL,
    SequentiallyConsistent = __ATOMIC_SEQ_CST,
};

template<typename T>
concept AtomicValue = IsIntegral<T> || IsEnum<T> || IsPointer<T>;

template<AtomicValue T>
class Atomic {
public:
    constexpr Atomic() = default;
    constexpr Atomic(T value)
        : m_value(value)
    {
    }

    Atomic(Atomic const&) = delete;
    Atomic& operator=(Atomic const&) = delete;

    [[nodiscard]] T load(MemoryOrder order = MemoryOrder::SequentiallyConsistent) const
    {
        T result;
        __atomic_load(&m_value, &result, static_cast<int>(order));
        return result;
    }

    void store(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent)
    {
        __atomic_store(&m_value, &value, static_cast<int>(order));
    }

    T exchange(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent)
    {
        T previous;
        __atomic_exchange(&m_value, &value, &previous, static_cast<int>(order));
        return previous;
    }

    // On failure, `expected` receives the current value.
    bool compare_exchange_strong(T& expected, T desired, MemoryOrder order = MemoryOrder::SequentiallyConsistent)
    {
        return __atomic_compare_exchange(&m_value, &expected, &desired, false, static_cast<int>(order), static_cast<int>(failure_order(order)));
    }

    T fetch_add(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) requires(IsIntegral<T>)
    {
        return __atomic_fetch_add(&m_value, value, static_cast<int>(order));
    }

    T fetch_sub(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) requires(IsIntegral<T>)
    {
        return __atomic_fetch_sub(&m_value, value, static_cast<int>(order));
    }

    T fetch_and(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) requires(IsIntegral<T>)
    {
        return __atomic_fetch_and(&m_value, value, static_cast<int>(order));
    }

    T fetch_or(T value, MemoryOrder order = MemoryOrder::SequentiallyConsistent) requires(IsIntegral<T>)
    {
        return __atomic_fetch_or(&m_value, value, static_cast<int>(order));
    }

    // For code that must hand the raw location to something else, such as
    // an exclusive-monitor wait loop.
    [[nodiscard]] T volatile* pointer() { return &m_value; }

private:
    // A failed compare-exchange only loads, so it cannot have release semantics.
    static constexpr MemoryOrder failure_order(MemoryOrder order)
    {
        if (order == MemoryOrder::Release)
            return MemoryOrder::Relaxed;
        if (order == MemoryOrder::AcquireRelease)
            return MemoryOrder::Acquire;
        return order;
    }

    alignas(sizeof(T)) T m_value {};
};

inline void atomic_thread_fence(MemoryOrder order)
{
    __atomic_thread_fence(static_cast<int>(order));
}

// Stops the compiler (not the CPU) from moving memory accesses across this point.
inline void compiler_barrier()
{
    asm volatile("" ::: "memory");
}

}

using Base::Atomic;
using Base::atomic_thread_fence;
using Base::compiler_barrier;
using Base::MemoryOrder;
