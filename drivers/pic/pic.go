package pic

import "github.com/vultureOS/vulture/kernel/sys"

const (
	PIC1_CMD  = 0x20
	PIC1_DATA = PIC1_CMD + 1
	PIC2_CMD  = 0xA0
	PIC2_DATA = PIC2_CMD + 1
)

func Init() {
	sys.Outb(PIC1_CMD, 0x11)
	sys.Outb(PIC2_CMD, 0x11)
}

func EnableIRQ(line uint16) {
	var port uint16 = PIC1_DATA
	if line >= 8 {
		port = PIC2_DATA
		line -= 8
	}

	sys.Outb(port, byte(sys.Inb(port)))
}
