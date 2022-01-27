package clock

import "github.com/vultureOS/vulture/kernel/sys"

type CmosTime struct {
	Second int
	Minute int
	Hour   int
	Day    int
	Month  int
	Year   int
}

func ReadCmosTime() CmosTime {
	var t CmosTime
	for {
		readCmosTime(&t)
		if bcdDecode(readCmosSecond()) == t.Second {
			break
		}
	}
	return t
}

func readCmosTime() int {

}

func readCmosSecond() int {
	return readCmosReg(0x00)
}

func readCmosReg(reg uint16) int {
	sys.Outb(0x70, 0x80|byte(reg))
	return int(sys.Inb(0x71))
}

func bcdDecode(v int) int {
	return v&0x0F + v/16*10
}
