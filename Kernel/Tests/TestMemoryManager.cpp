/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tests/KernelTest.h>

namespace Kernel {

KERNEL_TEST(memory, kernel_stacks_have_guards_and_are_reused)
{
    auto& mm = memory_manager();
    auto& table = mm.kernel_page_table();
    auto stack = mm.allocate_kernel_stack();
    KTEST_EXPECT(!stack.is_error());
    if (stack.is_error())
        return;
    auto [bottom, top] = stack.value();
    KTEST_EXPECT_EQ(top - bottom, MemoryLayout::kernel_stack_size);
    for (FlatPtr page = bottom; page < top; page += page_size) {
        *reinterpret_cast<u64 volatile*>(page) = page;
        u64 read_back = *reinterpret_cast<u64 volatile*>(page);
        KTEST_EXPECT_EQ(read_back, page);
        KTEST_EXPECT(table.translate(page).has_value());
    }
    // The page just below the stack is the guard.
    KTEST_EXPECT(!table.translate(bottom - page_size).has_value());

    mm.free_kernel_stack(stack.value());
    KTEST_EXPECT(!table.translate(bottom).has_value());

    auto again = mm.allocate_kernel_stack();
    KTEST_EXPECT(!again.is_error());
    if (!again.is_error()) {
        KTEST_EXPECT_EQ(again.value().bottom, bottom);
        mm.free_kernel_stack(again.value());
    }
}

KERNEL_TEST(memory, direct_map_and_kernel_image_translate)
{
    auto& mm = memory_manager();
    auto page = physical_memory().allocate_page(PageUse::General);
    KTEST_EXPECT(!page.is_error());
    if (page.is_error())
        return;
    auto* pointer = physical_to_virtual<u64>(page.value());
    KTEST_EXPECT(mm.kernel_page_table().translate(reinterpret_cast<FlatPtr>(pointer)) == page.value());
    KTEST_EXPECT(virtual_to_physical(pointer) == page.value());
    physical_memory().free_pages(page.value());

    // Code and data in the kernel image map to where the loader put them.
    static int s_data;
    auto data = reinterpret_cast<FlatPtr>(&s_data);
    KTEST_EXPECT(mm.kernel_page_table().translate(data) == mm.physical_address_of_kernel_image(&s_data));
}

KERNEL_TEST(memory, loader_memory_was_reclaimed)
{
    KTEST_EXPECT(memory_manager().reclaimed_bytes() > 0);
    KTEST_EXPECT(memory_manager().memory_map().size() > 0);
#if !defined(__x86_64__)
    KTEST_EXPECT(memory_manager().device_tree().size() > 0);
#endif
}

}
