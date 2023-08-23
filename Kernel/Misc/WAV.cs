/**
 * @file WAV.cs
 * @author Krisna Pranav
 * @brief WAV
 * @version 1.0 
 * @date 2023-08-22
 *
 * @copyright Copyright (c) 2022 - 2023 pranaOS Developers, Krisna Pranav
 *
 */

using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Vulture.Misc
{
    public static unsafe class WAV
    {
        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        public struct Header 
        {
            public uint ChunkID;
        }

        public static void Decode(byte[] WAV, out byte[] PCM, out Header header)
        {
            fixed (byte* PWAV = WAV)
            {
                Header* hdr = (Header*)PWAV;

                if (hdr->AudioFormat != 1)
                {
                    PCM = null;
                    header = default;
                    return;
                }
                PCM = new byte[hdr->Subchunk2Size];

                header = *hdr;
            }
        }
    }
}