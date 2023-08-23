using Vulture.Misc;

namespace Vulture
{
    public static unsafe class PageTable
    {
        public enum PageSize
        {
            Typical = 4096,
        }

        public static ulong* PML4;

        internal static void Initialize()
        {
            PML4 = (ulong*)SMP.SharedPageTable;

            Native.Stosb(PML4, 0, 0x1000);

            ulong i = 0;

            for (i = (ulong)PageSize.Typical; i < 1024 * 1024; i += (ulong)PageSize.Typical)
            {
                Map(i, i, PageSize.Typical);
            }

            Native.WriteCR3((ulong)PML4);
        }

        public static ulong* Next(ulong* Directory, ulong Entry)
        {
            ulong* p = null;

            if (((Directory[Entry]) & 0x01) != 0)
            {
                p = (ulong*)(Directory[Entry] & 0x000F_FFFF_FFFF_FFFF);
            }
            else 
            {
                p = (ulong*)Allocator.Allocate(0x1000);
                Native.Stosb(p, 0, 0x1000);

                Directory[Entry] = (((ulong)p) & 0x000F_FFFF_FFFF_FFFF);
            }

            return p;
        }
    }
}