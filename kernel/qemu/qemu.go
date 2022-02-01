package qemu

import (
	"github.com/vultureOS/vulture/kernel/sys"
)

const (
	qemuExitPort = 0x501
)

/* exit qemu port */
func Exit(code int) {
	sys.Outb(qemuExitPort, byte(code))
}
