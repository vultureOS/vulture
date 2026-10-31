/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Assertions.h>
#include <LibBase/Types.h>

// A doubly linked list whose links live inside the elements, so adding and
// removing never allocates. An element embeds one IntrusiveListNode per list
// it can be on:
//
//     struct Timer {
//         IntrusiveListNode list_node;
//     };
//     IntrusiveList<Timer, &Timer::list_node> timers;
//
// A node knows its list, so an element can unlink itself in O(1). Lists
// neither own nor destroy their elements, and cannot be copied or moved
// (nodes point back at them).

namespace Base {

class IntrusiveListBase;

class IntrusiveListNode {
public:
    IntrusiveListNode() = default;
    IntrusiveListNode(IntrusiveListNode const&) = delete;
    IntrusiveListNode& operator=(IntrusiveListNode const&) = delete;

    [[nodiscard]] bool is_in_list() const { return m_list != nullptr; }

    // Unlinks the element from whichever list it is on.
    inline void remove();

private:
    friend class IntrusiveListBase;

    IntrusiveListNode* m_next { nullptr };
    IntrusiveListNode* m_previous { nullptr };
    IntrusiveListBase* m_list { nullptr };
};

class IntrusiveListBase {
public:
    IntrusiveListBase() = default;
    IntrusiveListBase(IntrusiveListBase const&) = delete;
    IntrusiveListBase& operator=(IntrusiveListBase const&) = delete;

    [[nodiscard]] bool is_empty() const { return m_first == nullptr; }
    [[nodiscard]] size_t size() const { return m_size; }

protected:
    void link_before(IntrusiveListNode& node, IntrusiveListNode* before)
    {
        VERIFY(!node.is_in_list());
        node.m_list = this;
        node.m_next = before;
        node.m_previous = before ? before->m_previous : m_last;
        if (node.m_previous)
            node.m_previous->m_next = &node;
        else
            m_first = &node;
        if (before)
            before->m_previous = &node;
        else
            m_last = &node;
        ++m_size;
    }

    void unlink(IntrusiveListNode& node)
    {
        VERIFY(node.m_list == this);
        if (node.m_previous)
            node.m_previous->m_next = node.m_next;
        else
            m_first = node.m_next;
        if (node.m_next)
            node.m_next->m_previous = node.m_previous;
        else
            m_last = node.m_previous;
        node.m_next = nullptr;
        node.m_previous = nullptr;
        node.m_list = nullptr;
        --m_size;
    }

    [[nodiscard]] bool owns(IntrusiveListNode const& node) const { return node.m_list == this; }
    static IntrusiveListNode* next_of(IntrusiveListNode const& node) { return node.m_next; }

    IntrusiveListNode* m_first { nullptr };
    IntrusiveListNode* m_last { nullptr };
    size_t m_size { 0 };

private:
    friend class IntrusiveListNode;
};

inline void IntrusiveListNode::remove()
{
    VERIFY(m_list);
    m_list->unlink(*this);
}

template<typename T, IntrusiveListNode T::* member>
class IntrusiveList : public IntrusiveListBase {
public:
    class Iterator {
    public:
        explicit Iterator(IntrusiveListNode* node)
            : m_node(node)
        {
        }

        T& operator*() const { return *element_of(m_node); }
        T* operator->() const { return element_of(m_node); }
        Iterator& operator++()
        {
            m_node = next_of(*m_node);
            return *this;
        }
        bool operator==(Iterator const&) const = default;

    private:
        IntrusiveListNode* m_node;
    };

    void append(T& element) { link_before(element.*member, nullptr); }
    void prepend(T& element) { link_before(element.*member, m_first); }
    void insert_before(T& element, T& before) { link_before(element.*member, &(before.*member)); }
    void remove(T& element) { unlink(element.*member); }

    [[nodiscard]] bool contains(T const& element) const { return owns(element.*member); }

    [[nodiscard]] T* first() const { return m_first ? element_of(m_first) : nullptr; }
    [[nodiscard]] T* last() const { return m_last ? element_of(m_last) : nullptr; }

    [[nodiscard]] T* next(T const& element) const
    {
        auto* node = next_of(element.*member);
        return node ? element_of(node) : nullptr;
    }

    T* take_first()
    {
        auto* element = first();
        if (element)
            remove(*element);
        return element;
    }

    // Removing the current element while iterating is not supported; use
    // take_first() or next() for that.
    Iterator begin() const { return Iterator(m_first); }
    Iterator end() const { return Iterator(nullptr); }

private:
    static FlatPtr member_offset()
    {
        // The offset of `member` within T, found on storage that is never
        // constructed, so T needs no default constructor.
        union Storage {
            Storage() { }
            ~Storage() { }
            T object;
            char bytes[sizeof(T)];
        } storage;
        return reinterpret_cast<FlatPtr>(&(storage.object.*member)) - reinterpret_cast<FlatPtr>(&storage.object);
    }

    static T* element_of(IntrusiveListNode* node)
    {
        return reinterpret_cast<T*>(reinterpret_cast<FlatPtr>(node) - member_offset());
    }
};

}

using Base::IntrusiveList;
using Base::IntrusiveListNode;
