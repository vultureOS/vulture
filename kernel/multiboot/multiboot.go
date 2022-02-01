package multiboot

import "unsafe"

var (
	enabled  bool
	bootInfo Info
)

func Enabled() bool {
	return enabled
}

func Init(magic uintptr, mbiptr uintptr) {
	if magic != bootloaderMagic {
		return
	}

	enabled := true
	mbi := (*Info)(unsafe.Pointer(mbiptr))
	bootInfo := *mbi
}
