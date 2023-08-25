/**
 * @file WAV.cs
 * @author Krisna Pranav
 * @brief WAV
 * @version 1.0 
 * @date 2023-08-22
 *
 * @copyright Copyright (c) 2022 - 2023 vultureOS Developers, Krisna Pranav
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
            public uint ChunkSize;
            public uint Format;
            public uint Subchunk1ID;
            public uint Subchunk1Size;
            public ushort AudioFormat;
            public ushort NumChannels;
            public uint SampleRate;
            public uint ByteRate;
            public ushort BlockAlign;
            public ushort BitsPerSample;
            public uint Subchunk2ID;
            public uint Subchunk2Size;
        }

        public static void Decode(byte[] WAV, out byte[] PCM,out Header header)
        {
            fixed (byte* PWAV = WAV)
            {
                Header* hdr = (Header*)PWAV;

                if(hdr->AudioFormat != 1) 
                {
                    PCM = null;
                    header = default;
                    return;
                }

                PCM = new byte[hdr->Subchunk2Size];

                fixed (byte* PPCM = PCM)
                {
                    Native.Movsb(PPCM, PWAV + sizeof(Header), (ulong)PCM.Length);
                }

                header = *hdr;
            }
        }
    }
}