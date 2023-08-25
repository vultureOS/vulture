/**
 * @file Disk.cs
 * @author Krisna Pranav
 * @brief Disk
 * @version 1.0
 * @date 2023-08-25
 *
 * @copyright Copyright (c) 2022 - 2023 vultureOS Developers, Krisna Pranav
 *
*/

namespace Vulture.FS
{
    public abstract unsafe class Disk
    {
        public static Disk Instance;

        public Disk()
        {
            Instance = this;
        }

        public abstract bool Read(ulong sector, uint count, byte* data);
        public abstract bool Write(ulong sector, uint count, byte* data);
    }
}