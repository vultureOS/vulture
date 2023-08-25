/**
 * @file PIT.cs
 * @author Krisna Pranav
 * @brief PITs
 * @version 1.0
 * @date 2023-08-25
 *
 * @copyright Copyright (c) 2022 - 2023 vultureOS Developers, Krisna Pranav
 *
*/

using System;
using Vulture.Driver;
using Vulture.Misc;

namespace Vulture
{
    [Obsolete("Use ACPI Timer or Local APIC Timer")]
    public class PIT
    {
        public const int Clock = 1193192;

        public static void Initialize(int hz)
        {
            ushort timerCount = (ushort)(Clock / hz);

            Interrupts.EnableInterrupt(0x20);
        }
    }
}