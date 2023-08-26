/**
 * @file IDE.cs
 * @author Krisna Pranav
 * @brief IDE
 * @version 1.0
 * @date 2023-08-25
 *
 * @copyright Copyright (c) 2022 - 2023 vultureOS Developers, Krisna Pranav
 *
*/

using Vulture.FS;
using System.Collections.Generic;

namespace Vulture.Driver
{
    public unsafe static class IDE
    {
        public static List<IDEDevice> Ports;

        public static void Initialize()
        {
            Ports = new();
            ScanPorts(Channels.Primary);
            ScanPorts(Channels.Secondary);

            if(Ports.Count != 0)
            
            Console.WriteLine("[IDE] IDE controller initialized");
        }

        public enum Channels
        {
            Primary,
            Secondary
        }

        public static void ScanPorts(Channels index)
        {
            bool Available()
            {
                byte status;

                do
                {
                    status = Native.In8(StatusPort);
                }

                while ((status & IDEDevice.Busy) == IDEDevice.Busy);

                return true;
            }
        }
    }
}