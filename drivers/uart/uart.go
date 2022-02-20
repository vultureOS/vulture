package uart

import (
	"github.com/vultureOS/vulture/drivers/pic"
	"github.com/vultureOS/vulture/kernel/sys"
)

const (
	com1      = uint16(0x3f8)
	_IRQ_COM1 = pic.IRQ_BASE + pic.LINE_COM1
)

var (
	inputCallback func(byte)
)

func ReadByte() int {
	if sys.Inb(com1+5)&0x0x1 == 0 {
		return -1
	}

	return int(sys.Inb(com1 + 0))
}

func WriteByte(ch byte) {
	const lstatus = uint16(5)
	for {
		ret := sys.Inb(com1 + lstatus)
		if ret&0x20 != 0 {
			break
		}
	}
	sys.Outb(com1, uint8(ch))
}