package sys

const PtrSize = 4 << (^uintptr(0) >> 64)
const PageSize = 4 << 10

func Outb(port uint16, data byte)

func Inb(port uint16) byte

func Outl(port uint16, data uint32)

func Inl(port uint16) uint32

func Cli()

func Sti()

func Hlt()

func Cr2() uintptr

func Flags() uintptr
