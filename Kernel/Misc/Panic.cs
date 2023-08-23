/**
 * @file Panic.cs
 * @author Krisna Pranav
 * @brief Panic
 * @version 1.0
 * @date 2023-08-23
 *
 * @copyright Copyright (c) 2022 - 2023 pranaOS Developers, Krisna Pranav
 *
*/

using Vulture.Driver;
using System;

namespace Vulture.Misc
{
    public static class Panic
    {
        public static void Error(string msg, bool skippable = false )
        {
            LocalAPIC.SendAllInterrupt(0xFD);
            IDT.Disable();
            Framebuffer.TripleBuffered = false;
            
            Console.Write("PANIC: ");
            
            if (!skippable)
            {
                FrameBuffer.Update();
                for (;;);
            }
        }
    }
}