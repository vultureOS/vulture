/**
 * @file strings.cs
 * @author Krisna Pranav
 * @brief strings
 * @version 1.0
 * @date 2023-08-25
 *
 * @copyright Copyright (c) 2022 - 2023 vultureOS Developers, Krisna Pranav
 *
*/

namespace Vulture
{
    internal unsafe class strings
    {
        public static int strlen(byte* c) 
        {
            int i = 0;
            while (c[i] != 0) i++;
            return i;
        }
    }
}