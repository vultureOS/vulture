package logger

import (
	"fmt"
	"io"
	"os"

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
	if err != nil {
		l.logf("Create error")
	}
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
