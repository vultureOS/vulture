/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/New.h>
#include <LibBase/String.h>

// A hash table with open addressing and linear probing. Keys hash through
// Traits<K>; integers, StringView and String work out of the box.

namespace Base {

inline constexpr u64 hash_integer(u64 value)
{
    // The SplitMix64 finalizer: every input bit affects every output bit.
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

inline constexpr u64 hash_bytes(char const* bytes, size_t length)
{
    // FNV-1a.
    u64 hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < length; ++i) {
        hash ^= static_cast<u8>(bytes[i]);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

template<typename T>
struct Traits;

template<Integral T>
struct Traits<T> {
    static constexpr u64 hash(T value) { return hash_integer(static_cast<u64>(value)); }
    static constexpr bool equals(T a, T b) { return a == b; }
};

template<typename T>
struct Traits<T*> {
    static u64 hash(T* value) { return hash_integer(reinterpret_cast<FlatPtr>(value)); }
    static bool equals(T* a, T* b) { return a == b; }
};

template<>
struct Traits<StringView> {
    static constexpr u64 hash(StringView value) { return hash_bytes(value.characters_without_null_termination(), value.length()); }
    static constexpr bool equals(StringView a, StringView b) { return a == b; }
};

// String keys can be looked up by StringView, without allocating.
template<>
struct Traits<String> {
    static u64 hash(String const& value) { return Traits<StringView>::hash(value.view()); }
    static u64 hash(StringView value) { return Traits<StringView>::hash(value); }
    static bool equals(String const& a, String const& b) { return a == b; }
    static bool equals(String const& a, StringView b) { return a.view() == b; }
};

template<typename K, typename V, typename KeyTraits = Traits<K>>
class HashMap {
public:
    struct Entry {
        K key;
        V value;
    };

    HashMap() = default;
    HashMap(HashMap const&) = delete;
    HashMap& operator=(HashMap const&) = delete;

    HashMap(HashMap&& other)
        : m_buckets(exchange(other.m_buckets, nullptr))
        , m_capacity(exchange(other.m_capacity, 0))
        , m_size(exchange(other.m_size, 0))
        , m_used(exchange(other.m_used, 0))
    {
    }

    ~HashMap() { clear(); }

    [[nodiscard]] size_t size() const { return m_size; }
    [[nodiscard]] bool is_empty() const { return m_size == 0; }

    // Inserts or replaces.
    ErrorOr<void> try_set(K key, V value)
    {
        if ((m_used + 1) * 4 > m_capacity * 3)
            TRY(rehash(m_size * 2 < 8 ? 16 : m_size * 4));
        u64 hash = KeyTraits::hash(key);
        Bucket* tombstone = nullptr;
        for (size_t i = hash & (m_capacity - 1);; i = (i + 1) & (m_capacity - 1)) {
            auto& bucket = m_buckets[i];
            if (bucket.state == State::Used && KeyTraits::equals(bucket.entry()->key, key)) {
                bucket.entry()->value = move(value);
                return {};
            }
            if (bucket.state == State::Deleted && !tombstone)
                tombstone = &bucket;
            if (bucket.state == State::Empty) {
                auto& target = tombstone ? *tombstone : bucket;
                if (!tombstone)
                    ++m_used;
                new (target.storage) Entry { move(key), move(value) };
                target.state = State::Used;
                ++m_size;
                return {};
            }
        }
    }

    void set(K key, V value) { MUST(try_set(move(key), move(value))); }

    template<typename Key>
    [[nodiscard]] V* get(Key const& key)
    {
        auto* bucket = find(key);
        return bucket ? &bucket->entry()->value : nullptr;
    }

    template<typename Key>
    [[nodiscard]] V const* get(Key const& key) const
    {
        return const_cast<HashMap*>(this)->get(key);
    }

    template<typename Key>
    [[nodiscard]] bool contains(Key const& key) const
    {
        return const_cast<HashMap*>(this)->find(key) != nullptr;
    }

    template<typename Key>
    bool remove(Key const& key)
    {
        auto* bucket = find(key);
        if (!bucket)
            return false;
        bucket->entry()->~Entry();
        bucket->state = State::Deleted;
        --m_size;
        return true;
    }

    void clear()
    {
        for (size_t i = 0; i < m_capacity; ++i) {
            if (m_buckets[i].state == State::Used)
                m_buckets[i].entry()->~Entry();
        }
        operator delete(m_buckets);
        m_buckets = nullptr;
        m_capacity = m_size = m_used = 0;
    }

    template<typename Callback>
    void for_each(Callback callback)
    {
        for (size_t i = 0; i < m_capacity; ++i) {
            if (m_buckets[i].state == State::Used)
                callback(m_buckets[i].entry()->key, m_buckets[i].entry()->value);
        }
    }

    template<typename Callback>
    void for_each(Callback callback) const
    {
        for (size_t i = 0; i < m_capacity; ++i) {
            if (m_buckets[i].state == State::Used)
                callback(m_buckets[i].entry()->key, m_buckets[i].entry()->value);
        }
    }

private:
    enum class State : u8 {
        Empty,
        Used,
        Deleted,
    };

    struct Bucket {
        State state;
        alignas(Entry) u8 storage[sizeof(Entry)];
        Entry* entry() { return reinterpret_cast<Entry*>(storage); }
    };

    template<typename Key>
    Bucket* find(Key const& key)
    {
        if (m_capacity == 0)
            return nullptr;
        u64 hash = KeyTraits::hash(key);
        for (size_t probes = 0, i = hash & (m_capacity - 1); probes < m_capacity; ++probes, i = (i + 1) & (m_capacity - 1)) {
            auto& bucket = m_buckets[i];
            if (bucket.state == State::Empty)
                return nullptr;
            if (bucket.state == State::Used && KeyTraits::equals(bucket.entry()->key, key))
                return &bucket;
        }
        return nullptr;
    }

    ErrorOr<void> rehash(size_t minimum)
    {
        size_t capacity = 16;
        while (capacity < minimum)
            capacity *= 2;
        size_t bytes;
        if (__builtin_mul_overflow(capacity, sizeof(Bucket), &bytes))
            return Error::from_errno(12);
        auto* buckets = static_cast<Bucket*>(operator new(bytes, nothrow));
        if (!buckets)
            return Error::from_errno(12);
        for (size_t i = 0; i < capacity; ++i)
            buckets[i].state = State::Empty;
        auto* old_buckets = m_buckets;
        size_t old_capacity = m_capacity;
        m_buckets = buckets;
        m_capacity = capacity;
        m_size = m_used = 0;
        for (size_t i = 0; i < old_capacity; ++i) {
            if (old_buckets[i].state != State::Used)
                continue;
            auto* entry = old_buckets[i].entry();
            u64 hash = KeyTraits::hash(entry->key);
            for (size_t j = hash & (m_capacity - 1);; j = (j + 1) & (m_capacity - 1)) {
                if (m_buckets[j].state == State::Empty) {
                    new (m_buckets[j].storage) Entry(move(*entry));
                    m_buckets[j].state = State::Used;
                    ++m_size;
                    ++m_used;
                    break;
                }
            }
            entry->~Entry();
        }
        operator delete(old_buckets);
        return {};
    }

    Bucket* m_buckets { nullptr };
    size_t m_capacity { 0 };
    size_t m_size { 0 };
    size_t m_used { 0 }; // Used and deleted buckets: what probing has to skip.
};

}

using Base::HashMap;
using Base::Traits;
