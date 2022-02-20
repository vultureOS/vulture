package uart

import "github.com/vultureOS/vulture/drivers/pic"

const (
	com1      = uint16(0x3f8)
	_IRQ_COM1 = pic.IRQ_BASE + pic.LINE_COM1
)
