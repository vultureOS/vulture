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
    }
}