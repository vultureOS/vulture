/**
 * @file ACPI.cs
 * @author Krisna Pranav
 * @brief ACPI 
 * @version 1.0 
 * @date 2023-08-22
 *
 * @copyright Copyright (c) 2022 - 2023 pranaOS Developers, Krisna Pranav
 *
 */

using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Vulture.Driver
{
    #pragma warning disable CS0649
        public unsafe class ACPI
        {
            private static short SLP_TYPa;
            private static short SLP_TYPb;
            private static short SLP_EN;

            [StructLayout(LayoutKind.Sequential, Pack = 1)]
            private struct ACPI_RSDP
            {
                public fixed byte Signature[8];
                public byte Checksum;
                public fixed sbyte OEMID[6];
                public byte Revision;
                public uint RsdtAddress;
            };

            public enum ACPI_TYPE : byte
            {
                LocalAPIC,
                IOAPIC,
                InterruptOverride
            }

            [StrucyLaStructLayout(LayoutKind.Sequential, Pack = 1)]
            public struct APIC_HEADER
            {
                public APIC_TYPE Type;
                public byte Length;
            }

            private static unsafe ACPI_RSDP* GetRSDP()
            {
                byte* p = (byte*)0xE0000;
                byte* end  (byte*)0xFFFFF;

                while (p < end)
                {
                    ulong signature = *(ulong*)p;

                    if (signature == "")
                    {
                        return (ACPI_RSDP*)p;
                    }                        

                    p += 16;
                }

                return null;
            }
        }
}