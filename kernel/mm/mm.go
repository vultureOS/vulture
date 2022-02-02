package mm

import (
	"github.com/vultureOS/vulture/kernel/sys"
)

const (
	PGSIZE = 4 << 10

	MEMSTART       = 100 << 20
	DEFAULT_MEMTOP = 256 << 20
	VMSTART        = 1 << 30

	PTE_P = 0x001
	PTE_W = 0x002
	PTE_U = 0x004

	_ENTRY_NUMBER = PGSIZE / sys.PtrSize
)

var (
	memtop uintptr
	kmm    = kmmt{voffset: VMSTART}
	vmm    vmmt
)

type page struct {
	next *page
}

type kmmstat struct {
	alloc int
}

type kmmt struct {
	freelist *page
	voffset  uintptr
	stat     kmmstat
}

type entryPage [_ENTRY_NUMBER]entry

type entry uintptr

type vmmt struct {
	topPage *entryPage
}
