/*
 * Copyright (c) 2022, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0 License
 */

package filesystem

import "math/rand"

/* type block */
type zero struct{}
type random struct{}

func (z zero) Read(b []byte) (int, error) {
	for i := range b {
		b[i] = 0
	}
	return len(b), nil
}

func (r random) Read(b []byte) (int, error) {
	return rand.Read(b)
}
