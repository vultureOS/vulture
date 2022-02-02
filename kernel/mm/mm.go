package mm

import (
	"unsafe"

	"github.com/vultureOS/vulture/kernel/multiboot"
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

	kmm = kmmt{voffset: VMSTART}
	vmm vmmt
)

func pageEnable()

func lcr3(topPage *entryPage)

func throw(msg string)

func pageRoundUp(size uintptr) uintptr {
	return (size + PGSIZE - 1) &^ (PGSIZE - 1)
}

func pageRoundDown(v uintptr) uintptr {
	return v &^ (PGSIZE - 1)
}

func pageEntryIdx(v uintptr, lvl int) uintptr {
	return (v >> (12 + (lvl-1)*9)) & (_ENTRY_NUMBER - 1)
}

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

func (k *kmmt) sbrk(n uintptr) uintptr {
	p := k.voffset
	k.voffset = pageRoundUp(k.voffset + n)
	if k.voffset < p {
		throw("virtual memory address all used")
	}
	return p
}

func (k *kmmt) alloc() uintptr {
	r := k.freelist
	if r == nil {
		throw("kmemt.alloc")
	}
	k.stat.alloc++
	k.freelist = r.next
	return uintptr(unsafe.Pointer(r))
}

func (k *kmmt) freeRange(start, end uintptr) {
	p := pageRoundUp(start)
	for ; p+PGSIZE <= end; p += PGSIZE {
		k.free(p)
	}
}

func (k *kmmt) free(p uintptr) {
	if p%PGSIZE != 0 || p >= memtop {
		throw("kmemt.free")
	}
	r := (*page)(unsafe.Pointer(p))
	r.next = k.freelist
	k.freelist = r
}

type entryPage [_ENTRY_NUMBER]entry

type entry uintptr

func (p entry) present() bool {
	return p&PTE_P != 0
}

func (p entry) addr() uintptr {
	return uintptr(p) &^ 0xfff
}

func (p entry) entryPage() *entryPage {
	return (*entryPage)(unsafe.Pointer(p.addr()))
}

type vmmt struct {
	topPage *entryPage
}

func (v *vmmt) munmap(va, size uintptr) bool {
	p := pageRoundDown(va)
	last := pageRoundDown(va + size - 1)
	for ; p != last; p += PGSIZE {
		pte := v.walkpgdir(p, false)
		if pte == nil {
			return false
		}
		if !pte.present() {
			return false
		}
		kmm.free(pte.addr())
		*pte = 0
	}
	return true
}

func (v *vmmt) mmap(va, size, perm uintptr) bool {
	var pa uintptr
	p := pageRoundDown(va)
	last := pageRoundDown(va + size - 1)
	for {
		pte := v.walkpgdir(p, true)
		if pte == nil {
			return false
		}
		if pte.present() {
			throw("mmap remap")
		} else {
			pa = kmm.alloc()
			sys.Memclr(pa, PGSIZE)
			*pte = entry(pa | perm)
		}
		if p == last {
			break
		}
		p += PGSIZE
	}
	return true
}

func Sbrk(n uintptr) uintptr {
	return kmm.sbrk(n)
}

func Mmap(va, size uintptr) uintptr {
	if va == 0 {
		va = kmm.sbrk(size)
	}
	vmm.mmap(va, size, PTE_P|PTE_W|PTE_U)
	lcr3(vmm.topPage)
	return va
}

func Munmap(va, size uintptr) bool {
	ok := vmm.munmap(va, size)
	lcr3(vmm.topPage)
	return ok
}

func Fixmap(va, pa, size uintptr) {
	vmm.fixmap(va, pa, size, PTE_P|PTE_W|PTE_U)
	lcr3(vmm.topPage)
}

func Alloc() uintptr {
	ptr := kmm.alloc()
	buf := sys.UnsafeBuffer(ptr, PGSIZE)
	for i := range buf {
		buf[i] = 0
	}
	return ptr
}

func (v *vmmt) fixmap(va, pa, size, perm uintptr) bool {
	p := pageRoundDown(va)
	last := pageRoundDown(va + size - 1)
	for {
		pte := v.walkpgdir(p, true)
		if pte == nil {
			return false
		}
		if pte.present() {
			throw("fixmap remap")
		}
		*pte = entry(pa | perm)
		if p == last {
			break
		}
		p += PGSIZE
		pa += PGSIZE
	}
	return true
}

func (v *vmmt) walkpglvl(pg *entryPage, va uintptr, lvl int, alloc bool) *entry {
	idx := pageEntryIdx(va, lvl)
	if int(idx) >= len(pg) {
		throw("bad page index")
	}
	pe := &pg[idx]
	if lvl == 1 {
		return pe
	}

	if pe.present() {
		return pe
	}

	if !alloc {
		return nil
	}

	addr := kmm.alloc()
	if addr == 0 {
		return nil
	}
	sys.Memclr(addr, PGSIZE)
	*pe = entry(addr | PTE_P | PTE_W | PTE_U)
	return pe
}

func (v *vmmt) walkpgdir(va uintptr, alloc bool) *entry {
	epg := v.topPage
	var pe *entry
	for i := 4; i >= 1; i-- {
		pe = v.walkpglvl(epg, va, i, alloc)
		if pe == nil {
			return nil
		}
		epg = pe.entryPage()
	}
	return pe
}

func findMemTop() uintptr {
	if !multiboot.Enabled() {
		return DEFAULT_MEMTOP
	}
	var top uintptr
	for _, e := range multiboot.BootInfo.MmapEntries() {
		if e.Type != multiboot.MemoryAvailable {
			continue
		}
		ptop := e.Addr + e.Len
		if ptop > VMSTART {
			ptop = VMSTART
		}
		if top < uintptr(ptop) {
			top = uintptr(ptop)
		}
	}
	if top == 0 {
		return DEFAULT_MEMTOP
	}
	return top
}

func Init() {
	memtop = findMemTop()
	kmm.voffset = VMSTART
	kmm.freeRange(MEMSTART, memtop)

	vmm.topPage = (*entryPage)(unsafe.Pointer(kmm.alloc()))
	sys.Memclr(uintptr(unsafe.Pointer(vmm.topPage)), PGSIZE)

	vmm.fixmap(4096, 4096, memtop-4096, PTE_P|PTE_W|PTE_U)

	lcr3(vmm.topPage)
	pageEnable()
}
