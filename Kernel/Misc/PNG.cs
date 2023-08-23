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


        [DllImport("*")]
        public static extern void lodepng_decode_memory(out uint* _out, out uint w, out uint h, byte* _in, int insize, LodePNGColorType colortype, uint bitdepth);
    }
}