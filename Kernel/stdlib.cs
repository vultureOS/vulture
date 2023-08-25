/**
 * @file stdlib.cs
 * @author Krisna Pranav
 * @brief stdlib
 * @version 1.0
 * @date 2023-08-25
 *
 * @copyright Copyright (c) 2022 - 2023 pranaOS Developers, Krisna Pranav
 *
*/

using System.Runtime;

namespace Vulture
{
    public static unsafe class stdlib
    {
        [RuntimeExport("malloc")]
        public static void* malloc(ulong size)
        {
            return (void*)Allocator.Allocate(size);
        }

        [RuntimeExport("free")]
        public static void free(void* ptr)
        {
            Allocator.Free((System.IntPtr)ptr);
        }

        [RuntimeExport("realloc")]
        public static void* realloc(void* ptr, ulong size)
        {
            return (void*)Allocator.Reallocate((System.IntPtr)ptr, size);
        }

        [RuntimeExport("calloc")]
        public static void* calloc(ulong num, ulong size)
        {
            void* ptr = (void*)Allocator.Allocate(num * size);
            return ptr;
        }

        [RuntimeExport("kfree")]
        public static void kfree(void* ptr)
        {
            Allocator.Free((System.IntPtr)ptr);
        }

        [RuntimeExport("krealloc")]
        public static void krealloc(void* ptr, ulong size)
        {
            return (void*)Allocator.Reallocate((System.IntPtr)ptr, size);
        }

        [runtimeExport("kcalloc")]
        public static void* kcalloc(ulong num, ulong size)
        {
           void* ptr = (void*)Allocator.Allocate(num * size);
           Native.Stosb(ptr, 0, num * size);
           return ptr; 
        }
    }
}