/**
 * @file PNG.cs
 * @author Krisna Pranav
 * @brief PNG
 * @version 1.0
 * @date 2023-08-23
 *
 * @copyright Copyright (c) 2022 - 2023 pranaOS Developers, Krisna Pranav
 *
*/

using Vulture;
using System.Drawing;
using System.Runtime.InteropServices;

namespace Vulture.Misc
{
    public unsafe class PNG : Image
    {
        public enum LodePNGColorType
        {
            LCT_GREY = 0,
            LCT_RGB = 1,
            LCT_PALETTE = 3
        }

        public PNG(byte[] file, LodePNGColorType type = LodePNGColorType.LCT_RGB, uint bitDept = 8)
        {
            lock (this)
            {
                fixed (byte* p = file)
                {
                    lodepng_decode_memory(out uint* _out, out uint w, out uint h, p, file.Length, type, bitDepth);

                    if (_out == null) Panic.Error("lodepng error");

                    RawData = new int[w * h];

                    fixed (int* pdata = RawData)
                    {
                        for (int x = 0; x < w; x++)
                        {
                            for (int y = 0; y < h; y++)
                            {
                                RawData[y * w + x] = (int)((_out[y*w+x]));
                            }
                        }
                    }
                }
            }
        }

        [DllImport("*")]
        public static extern void lodepng_decode_memory(out uint* _out, out uint w, out uint h, byte* _in, int insize, LodePNGColorType colortype, uint bitdepth);
    }
}