/**
 * @file stdio.cs
 * @author Krisna Pranav
 * @brief stdio
 * @version 1.0
 * @date 2023-08-25
 *
 * @copyright Copyright (c) 2022 - 2023 vultureOS Developers, Krisna Pranav
 *
*/

using Vulture.FS;
using Vulture.Misc;
using System.Runtime;
using System.Runtime.InteropServices;

namespace Vulture
{
    internal unsafe class stdio
    {
        [RuntimeExport("_putchar")]
        public static void _putchar(byte chr)
        {
            if (chr == '\n')
            {
                Console.WriteLine();
            } else {
                Console.Write((char)chr);
            }
        }

        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        public struct FILE
        {
            public byte* DATA;
            public long OFFSET;
            public long LENGTH;
        }

        public enum SEEK
        {
            SET,
            CUR,
            END
        }
    }
}