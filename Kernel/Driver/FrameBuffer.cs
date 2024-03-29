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
    public static unsafe class Framebuffer
    {
        public static ushort Width;
        public static ushort Height;

        public static uint* VideoMemory { get; private set; }

        public static uint* FirstBuffer;
        public static uint* SecondBuffer;

        public static Graphics Graphics;

        static bool _TripleBuffered = false;

        public static bool TripleBuffered 
        {
            get 
            {
                return _TripleBuffered;
            }
            set 
            {
                if (Graphics == null) return;
                if (_TripleBuffered == value) return;

                Graphics.Clear(0x0);
                Graphics.VideoMemory = value ? FirstBuffer : VideoMemory;
                _TripleBuffered = value;
                if (!_TripleBuffered)
                {
                    Console.Clear();
                }
            }
        }

        public static void Update()
        {
            if (TripleBuffered)
            {
                for(int i = 0; i < Width * Height; i++) 
                {
                    if(FirstBuffer[i] != SecondBuffer[i]) 
                    {
                        VideoMemory[i] = FirstBuffer[i];
                    }
                }
                Native.Movsd(SecondBuffer, FirstBuffer, (ulong)(Width * Height));
            }
            if(Graphics != null) Graphics.Update();
        }

        public static void Initialize(ushort XRes, ushort YRes,uint* FB)
        {
            Width = XRes;
            Height = YRes;

            FirstBuffer = (uint*)Allocator.Allocate((ulong)(XRes * YRes * 4));
            SecondBuffer = (uint*)Allocator.Allocate((ulong)(XRes * YRes * 4));

            Native.Stosd(FirstBuffer, 0, (ulong)(XRes * YRes));
            Native.Stosd(SecondBuffer, 0, (ulong)(XRes * YRes));

            Control.MousePosition.X = XRes / 2;
            Control.MousePosition.Y = YRes / 2;
            
            Graphics = new Graphics(Width, Height, FB);
            
            VideoMemory = FB;

            Console.Clear();
        }
    }
}