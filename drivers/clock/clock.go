/*
 * Copyright (c) 2022, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0 License
 */

package clock

import (
	"time"

	"github.com/vultureOS/vulture/kernel/sys"
)

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

func (c *CmosTime) Time() time.Time {
	return time.Date(c.Year, time.Month(c.Month), c.Day, c.Hour, c.Minute, c.Second, 0, time.UTC)
}

func readCmosTime(t *CmosTime) {
	t.Year = bcdDecode(readCmosReg(0x09)) + bcdDecode(readCmosReg(0x32))*100
	t.Month = bcdDecode(readCmosReg(0x08))
	t.Day = bcdDecode(readCmosReg(0x07))
	t.Hour = bcdDecode(readCmosReg(0x04))
	t.Minute = bcdDecode(readCmosReg(0x02))
	t.Second = bcdDecode(readCmosReg(0x00))
}

func readCmosSecond() int {
	return readCmosReg(0x00)
}

func bcdDecode(v int) int {
	return v&0x0F + v/16*10
}

func readCmosReg(reg uint16) int {
	sys.Outb(0x70, 0x80|byte(reg))
	return int(sys.Inb(0x71))
}
