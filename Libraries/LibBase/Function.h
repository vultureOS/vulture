/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Assertions.h>
#include <LibBase/New.h>
#include <LibBase/StdLibExtras.h>
#include <LibBase/TypeTraits.h>

// Function<R(Args...)>: any callable (a lambda with captures, a function
// pointer) behind one type, for callbacks. It owns the callable (moved in),
// can be moved but not copied, and may be empty.

namespace Base {

template<typename>
class Function;

template<typename R, typename... Args>
class Function<R(Args...)> {
public:
    Function() = default;
    Function(nullptr_t) { }

    template<typename Callable>
    requires(!IsSame<RemoveCVReference<Callable>, Function> && requires(Decay<Callable>& callable, Args... arguments) { callable(arguments...); })
    Function(Callable&& callable)
    {
        auto* holder = new (nothrow) Holder<Decay<Callable>>(forward<Callable>(callable));
        VERIFY(holder);
        m_callable = holder;
    }

    Function(Function&& other)
        : m_callable(exchange(other.m_callable, nullptr))
    {
    }

    Function& operator=(Function&& other)
    {
        if (this != &other) {
            delete m_callable;
            m_callable = exchange(other.m_callable, nullptr);
        }
        return *this;
    }

    Function& operator=(nullptr_t)
    {
        delete m_callable;
        m_callable = nullptr;
        return *this;
    }

    Function(Function const&) = delete;
    Function& operator=(Function const&) = delete;

    ~Function() { delete m_callable; }

    R operator()(Args... arguments) const
    {
        VERIFY(m_callable);
        return m_callable->call(forward<Args>(arguments)...);
    }

    explicit operator bool() const { return m_callable != nullptr; }

private:
    struct CallableBase {
        virtual ~CallableBase() = default;
        virtual R call(Args...) = 0;
    };

    template<typename Callable>
    struct Holder final : CallableBase {
        template<typename From>
        explicit Holder(From&& from)
            : callable(forward<From>(from))
        {
        }
        R call(Args... arguments) override { return callable(forward<Args>(arguments)...); }
        Callable callable;
    };

    CallableBase* m_callable { nullptr };
};

}

using Base::Function;
