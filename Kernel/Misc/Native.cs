/**
 * @file Native.cs
 * @author Krisna Pranav
 * @brief Native
 * @version 1.0
 * @date 2023-08-23
 *
 * @copyright Copyright (c) 2022 - 2023 pranaOS Developers, Krisna Pranav
 *
*/

using Vulture.Misc;
using System;
using System.Runtime.InteropServices;

static unsafe class Native
{
    [DllImport("*")]
    public static extern CPUID* CPUID(int index);

    [DllImport("*")]
    public static extern void Insb(ushort port, byte* data, ulong count);

    [DllImport("*")]
    public static extern void Insw(ushort port, ushort* data, ulong count);

    [DllImport("*")]
    public static extern void Sti();

    [DllImport("*")]
    public extern static void Invlpg(ulong physicalAddress);

    [DllImport("*")]
    public extern static void Nop();

    [DllImport("*")]
    public extern static void Fxsave64(void* ptr);
}