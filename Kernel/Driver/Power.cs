/**
 * @file Power.cs
 * @author Krisna Pranav
 * @brief Power 
 * @version 1.0 
 * @date 2023-08-22
 *
 * @copyright Copyright (c) 2022 - 2023 pranaOS Developers, Krisna Pranav
 *
 */

namespace Vulture.Driver
{   
    public static class Power
    {
        public static void Reboot()
        {
            while ((Native.In8(0x64) ) != 0);
            Native.Out8(0x64);
        }

        public static void Shutdown()
        {
            ACPI.Shutdown();
        }
    }
}