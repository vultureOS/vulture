/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/FileSystem.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Memory/PhysicalAddress.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

namespace Kernel {

class TmpFSInode;

// A filesystem in memory. File contents are pages (reference-counted, like
// user memory), so a file can be any size and holes cost nothing.
class TmpFS final : public FileSystem {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<TmpFS>> create();

    [[nodiscard]] StringView name() const override { return "tmpfs"_sv; }
    [[nodiscard]] NonnullRefPtr<Inode> root_inode() override;
    ErrorOr<void> rename(Inode& old_parent, StringView old_name, Inode& new_parent, StringView new_name) override;

    [[nodiscard]] Mutex& lock() { return m_lock; }
    [[nodiscard]] InodeIndex allocate_index() { return m_next_index.fetch_add(1); }

private:
    TmpFS() = default;

    Mutex m_lock;
    RefPtr<TmpFSInode> m_root;
    Atomic<InodeIndex> m_next_index { 2 };
};

class TmpFSInode final : public Inode {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<TmpFSInode>> create(TmpFS&, InodeType, u32 permissions);
    ~TmpFSInode() override;

    [[nodiscard]] InodeMetadata metadata() const override;
    ErrorOr<size_t> read(u64 offset, size_t count, IOBuffer const&) override;
    ErrorOr<size_t> write(u64 offset, size_t count, IOBuffer const&) override;
    ErrorOr<void> truncate(u64 size) override;
    ErrorOr<NonnullRefPtr<Inode>> lookup(StringView name) override;
    ErrorOr<PhysicalAddress> mapped_page(u64 index) override;
    ErrorOr<void> traverse(DirectoryVisitor&) override;
    ErrorOr<NonnullRefPtr<Inode>> create_child(StringView name, InodeType, u32 permissions, StringView link_target) override;
    ErrorOr<void> remove_child(StringView name) override;
    ErrorOr<void> add_child(StringView name, Inode&) override;
    ErrorOr<size_t> read_link(Bytes) override;
    ErrorOr<void> set_permissions(u32) override;
    ErrorOr<void> set_owner(u32 uid, u32 gid) override;
    ErrorOr<void> set_times(i64 access_time, i64 modification_time) override;

private:
    void mark_modified();
    friend class TmpFS;

    struct Child {
        String name;
        NonnullRefPtr<TmpFSInode> inode;
    };

    TmpFSInode(TmpFS&, InodeIndex, InodeType, u32 permissions);
    TmpFS& tmpfs() const { return static_cast<TmpFS&>(file_system()); }
    // The page holding byte `offset`, allocated if `allocate`; referenced
    // for the caller, which unrefs it.
    ErrorOr<PhysicalAddress> page_at(u64 offset, bool allocate);
    void free_pages_from(size_t first_page);
    Child* find_child(StringView name);

    InodeMetadata m_metadata;
    Vector<PhysicalAddress> m_pages;
    Vector<Child> m_children;
    TmpFSInode* m_parent { nullptr };
    String m_link_target;
};

}
