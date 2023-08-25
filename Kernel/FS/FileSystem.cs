/**
 * @file FileSystem.cs
 * @author Krisna Pranav
 * @brief FileSystem
 * @version 1.0
 * @date 2023-08-25
 *
 * @copyright Copyright (c) 2022 - 2023 vultureOS Developers, Krisna Pranav
 *
*/

using System.Collections.Generic;

namespace Vulture
{
    public class FileInfo
    {
        public string Name;
        public FileAttribute Attribute;

        public ulong Param0;
        public ulong Param1;

        public override void Dispose()
        {
            Name.Dispose();
            base.Dispose();
        }
    }

    public enum FileAttribute: byte
    {
        ReadOnly = 0x01,
        Hidden = 0x02,
        System = 0x04,
        Directory = 0x10,
        Archive = 0x20,
    }

    public static class File
    {
        public static FileSystem Instance;

        public static List<FileInfo> GetFiles(string Directory) => Instance.GetFiles(Directory);

        public static byte[] ReadAllBytes(string name) => Instance.ReadAllBytes(name);
    }

    public abstract class FileSystem
    {
        public FileSystem()
        {
            File.Instance = this;
        }

        public const int SectorSize = 512;

        public string ulong SizeToSec(ulong size)
        {
            return ((size - (size % SectorSize)) / SectorSize)
        }

        public static bool IsInDirectory(string fname, string dir)
        {
            if (fname.Length < dir.Length) return false;

            for (int i = 0; i < fname.Length; i++)
            {
                if (i < dir.Length)
                {
                    if (dir[i] != fname[i]) return false;
                }
                else
                {
                    if (fname[i] == '/') return false;
                }
            }

            return true;
        }

        public abstract List<FileInfo> GetFiles(string dir);
        public abstract void Delete(string Name);

        public abstract byte[] ReadAllBytes(string Name);

        public abstract void WriteAllBytes(string Name, byte[] Content);

        public abstract void Format();
    }
}