namespace Vulture
{
    public static class RTC
    {
        private static byte B;

        public static byte Get(byte index)
        {
            Native.Out8(0x70, index);
            byte result = Native.In8(0x71);

            return result;
        }

        public static byte Second
        {
            get
            {
                B = Get(0);
                return (byte)((B& 0x0F) + ((B/16) * 10));
            }
        }        
        
    }
}