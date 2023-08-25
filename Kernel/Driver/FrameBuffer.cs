/**
 * @file FrameBuffer.cs
 * @author Krisna Pranav
 * @brief FrameBuffer
 * @version 1.0
 * @date 2023-08-25
 *
 * @copyright Copyright (c) 2022 - 2023 vultureOS Developers, Krisna Pranav
 *
*/

using Vulture.Driver;
using Vulture.Graph;
using Vulture.Misc;
using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;

namespace Vulture
{
    public static unsafe class FrameBuffer
    {
        public static ushort Width;
        public static ushort Height;

        public static uint* VideoMemory { get; private set; }

        public static uint* FirstBuffer;
        public static uint* SecondBuffer;

        public static Graphics Graphics;

        static bool _TripleBuffered = false;

        
    }
}