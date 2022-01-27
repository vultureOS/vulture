/*
 * Copyright (c) 2022, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0 License
 */

package logger

import (
	"fmt"
	"io"
	"os"
	"time"

	"github.com/spf13/afero"
)

type logger struct {
	w       io.Writer
	backend afero.Fs
}

func New(w io.Writer, fs afero.Fs) afero.Fs {
	return &logger{
		w:       w,
		backend: fs,
	}
}

func (l *logger) logf(fmtstr string, args ...interface{}) {
	fmtstr = fmt.Sprintf("[%s] %s\n", l.backend.Name(), fmtstr)
	fmt.Fprintf(l.w, fmtstr, args...)
}

func (l *logger) Create(name string) (afero.File, error) {
	ret, err := l.backend.Create(name)
	l.logf("Create(%s) %v", name, err)
	return ret, err
}

func (l *logger) Mkdir(name string, perm os.FileMode) error {
	err := l.backend.Mkdir(name, perm)
	l.logf("Mkdir(%s, %s) %v", name, perm, err)
	return err
}

func (l *logger) MkdirAll(path string, perm os.FileMode) error {
	err := l.backend.MkdirAll(path, perm)
	l.logf("MkdirAll(%s, %s) %v", path, perm, err)
	return err
}

func (l *logger) Open(name string) (afero.File, error) {
	ret, err := l.backend.Open(name)
	l.logf("Open(%s) %v", name, err)
	return ret, err
}

func (l *logger) OpenFile(name string, flag int, perm os.FileMode) (afero.File, error) {
	ret, err := l.backend.OpenFile(name, flag, perm)
	l.logf("OpenFile(%s, %x, %s) %v", name, flag, perm, err)
	return ret, err
}

func (l *logger) Remove(name string) error {
	err := l.backend.Remove(name)
	l.logf("Remove(%s) %v", name, err)
	return err
}

func (l *logger) RemoveAll(path string) error {
	err := l.backend.RemoveAll(path)
	l.logf("RemoveAll(%s) %v", path, err)
	return err
}

func (l *logger) Rename(oldname string, newname string) error {
	err := l.backend.Rename(oldname, newname)
	l.logf("Rename(%s, %s) %v", oldname, newname, err)
	return err
}

func (l *logger) Stat(name string) (os.FileInfo, error) {
	ret, err := l.backend.Stat(name)
	l.logf("Stat(%s) %v", name, err)
	return ret, err
}

func (l *logger) Name() string {
	return "logger"
}

func (l *logger) Chmod(name string, mode os.FileMode) error {
	err := l.backend.Chmod(name, mode)
	l.logf("Chmod(%s, %s) %v", name, mode, err)
	return err
}

func (l *logger) Chtimes(name string, atime time.Time, mtime time.Time) error {
	err := l.backend.Chtimes(name, atime, mtime)
	l.logf("Chtimes(%s, %d, %d) %v", name, atime.Unix(), mtime.Unix(), err)
	return err
}
