package multiboot

const (
	bootloaderMagic = 0x00000
)

const (
	MemoryAvailable = 1 << iota
	MemoryReserved
	MemoryACPIReclaimable
)

type Flag uint32

type Info struct {
	Flags    Flag
	MemLower uint32
}

func (i *Info) MmapEntries() []MmapEntry {
	return i.MmapEntries()
}

type MmapEntry struct {
	Size uint32
	Addr uint64
	Len  uint64
	Type uint32
}
