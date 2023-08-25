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

        [RuntimeExport("fopen")]
        public  static FILE* fopen(byte* name, byte* mode)
        {
            string sname = string.FromASCII((System.IntPtr)name, strings.strlen(name));
            FILE file = new FILE();


            byte[] buffer = file.ReadAllBytes(sname);

            if (buffer == null)
            {
                Panic.Error("fopen: file not found");
            }

            file.DATA = (byte*)Allocator.Allocate((ulong)buffer.Length);
            file.LENGTH = buffer.Length;
            buffer.Dispose();
            sname.Dispose();

            return &file;
        }
    }
}