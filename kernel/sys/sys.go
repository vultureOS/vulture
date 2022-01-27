package sys

import "unsafe"

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

func unsafeBuffer(p uintptr, n int) []byte {
	return (*[1 << 30]byte)(unsafe.Pointer(p))[:n]
}

func memClr(p uintptr, n int) {
	s := (*(*[1 << 30]byte)(unsafe.Pointer(p)))[:n]

	for i := range s {
		s[i] = 0
	}
}

func fxSave(addr uintptr)

func setAx(val uintptr)

func CS() uintptr
