package vbe

import (
	"image"
	"image/draw"
)

type View struct {
	buffer *image.RGBA
}

func (v *View) Canvas() draw.Image {
	return v.buffer
}

func (v *View) Clear() {
	for i := range v.buffer.Pix {
		v.buffer.Pix[i] = 0
	}
}
