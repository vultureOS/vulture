/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Lock.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// Small blocks (up to 16 KiB with their header) come from size classes
// carved out of 64 KiB chunks; larger ones are mappings of their own.
// Every block starts with a 16-byte header, which keeps payloads 16-byte
// aligned and lets free() catch bad and double frees. One lock guards the
// free lists.

namespace {

constexpr uint32_t used_magic = 0x6d616c6c; // "mall"
constexpr uint32_t free_magic = 0x66726565; // "free"
constexpr size_t header_size = 16;
constexpr size_t class_count = 10; // 32 .. 16384 bytes
constexpr size_t smallest_shift = 5;
constexpr size_t chunk_size = 64 * 1024;
constexpr uint32_t large_class = 0xff;
constexpr size_t page_size = 4096;

struct Header {
    uint32_t magic;
    uint32_t size_class;
    size_t size; // Payload capacity.
};

static_assert(sizeof(Header) == header_size);

struct FreeBlock {
    Header header;
    FreeBlock* next;
};

FreeBlock* s_free_lists[class_count];
LibC::Lock s_lock;

size_t class_for(size_t total)
{
    size_t size_class = 0;
    while ((size_t(1) << (size_class + smallest_shift)) < total)
        ++size_class;
    return size_class;
}

[[noreturn]] void corrupted(char const* what, void* pointer)
{
    fprintf(stderr, "malloc: %s at %p\n", what, pointer);
    abort();
}

bool refill(size_t size_class)
{
    void* chunk = mmap(nullptr, chunk_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (chunk == MAP_FAILED)
        return false;
    size_t block_size = size_t(1) << (size_class + smallest_shift);
    for (size_t offset = 0; offset + block_size <= chunk_size; offset += block_size) {
        auto* block = reinterpret_cast<FreeBlock*>(static_cast<char*>(chunk) + offset);
        block->header = { free_magic, static_cast<uint32_t>(size_class), block_size - header_size };
        block->next = s_free_lists[size_class];
        s_free_lists[size_class] = block;
    }
    return true;
}

}

extern "C" {

void* malloc(size_t size)
{
    if (size == 0)
        size = 1;
    if (size > SIZE_MAX - page_size - header_size) {
        errno = ENOMEM;
        return nullptr;
    }
    size_t total = size + header_size;
    if (total > (size_t(1) << (class_count - 1 + smallest_shift))) {
        size_t mapping = (total + page_size - 1) & ~(page_size - 1);
        void* memory = mmap(nullptr, mapping, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) {
            errno = ENOMEM;
            return nullptr;
        }
        auto* header = static_cast<Header*>(memory);
        *header = { used_magic, large_class, mapping - header_size };
        return static_cast<char*>(memory) + header_size;
    }
    size_t size_class = class_for(total);
    LibC::LockGuard guard(s_lock);
    if (!s_free_lists[size_class] && !refill(size_class)) {
        errno = ENOMEM;
        return nullptr;
    }
    FreeBlock* block = s_free_lists[size_class];
    if (block->header.magic != free_magic)
        corrupted("corrupted free list", block);
    s_free_lists[size_class] = block->next;
    block->header.magic = used_magic;
    return reinterpret_cast<char*>(block) + header_size;
}

void free(void* pointer)
{
    if (!pointer)
        return;
    auto* header = reinterpret_cast<Header*>(static_cast<char*>(pointer) - header_size);
    if (header->magic == free_magic)
        corrupted("double free", pointer);
    if (header->magic != used_magic)
        corrupted("free of a pointer malloc did not return", pointer);
    if (header->size_class == large_class) {
        munmap(header, header->size + header_size);
        return;
    }
    if (header->size_class >= class_count)
        corrupted("corrupted block header", pointer);
    LibC::LockGuard guard(s_lock);
    // (Again, now that no other thread can be freeing it.)
    if (header->magic != used_magic)
        corrupted("double free", pointer);
    auto* block = reinterpret_cast<FreeBlock*>(header);
    block->header.magic = free_magic;
    block->next = s_free_lists[header->size_class];
    s_free_lists[header->size_class] = block;
}

void __malloc_fork_lock()
{
    s_lock.lock();
}

void __malloc_fork_unlock()
{
    s_lock.unlock();
}

void* calloc(size_t count, size_t size)
{
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return nullptr;
    }
    void* pointer = malloc(total);
    if (pointer)
        memset(pointer, 0, total);
    return pointer;
}

void* realloc(void* pointer, size_t size)
{
    if (!pointer)
        return malloc(size);
    if (size == 0) {
        free(pointer);
        return nullptr;
    }
    auto* header = reinterpret_cast<Header*>(static_cast<char*>(pointer) - header_size);
    if (header->magic != used_magic)
        corrupted("realloc of a pointer malloc did not return", pointer);
    if (size <= header->size)
        return pointer;
    void* resized = malloc(size);
    if (!resized)
        return nullptr;
    memcpy(resized, pointer, header->size);
    free(pointer);
    return resized;
}
}
