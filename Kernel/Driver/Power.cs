/**
 * @file Power.cs
 * @author Krisna Pranav
 * @brief Power 
 * @version 1.0 
 * @date 2023-08-22
 *
 * @copyright Copyright (c) 2022 - 2023 vultureOS Developers, Krisna Pranav
 *
 */

namespace MOOS.Driver
{
    public static class Power
    {   
        /// <summary>
        ///  reboot functionalitiy sends out ps2 
        /// </summary>
        public static void Reboot()
        {
            while ((Native.In8(0x64) & 0x02) != 0) ;
            Native.Out8(0x64, 0xFE);
            Native.Hlt();
        }

        /// <summary>
        /// shutdown
        /// </summary>
        public static void Shutdown()
        {
            ACPI.Shutdown();
        }
    }
}