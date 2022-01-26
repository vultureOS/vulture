package chdir

import (
	"errors"
	"os"
	"path/filepath"
	"time"

	"github.com/spf13/afero"
)

type Chdirfs struct {
	dir     string
	backend afero.Fs
}

func New(backend afero.Fs) *Chdirfs {
	return &Chdirfs{
		dir:     "/",
		backend: backend,
	}
}

func (c *Chdirfs) Chdir(dir string) error {
	name := c.name(dir)
	fi, err := c.backend.Stat(name)
	if err != nil {
		return err
	}
	if !fi.IsDir() {
		return errors.New("not a dir")
	}
	c.dir = name
	return nil
}

func (c *Chdirfs) name(name string) string {
	if filepath.IsAbs(name) {
		return name
	}
	return filepath.Join(c.dir, name)
}

func (c *Chdirfs) Create(name string) (afero.File, error) {
	return c.backend.Create(c.name(name))
}

func (c *Chdirfs) Mkdir(name string, perm os.FileMode) error {
	return c.backend.Mkdir(c.name(name), perm)
}

func (c *Chdirfs) MkdirAll(path string, perm os.FileMode) error {
	return c.backend.MkdirAll(c.name(path), perm)
}

func (c *Chdirfs) Open(name string) (afero.File, error) {
	return c.backend.Open(c.name(name))
}

func (c *Chdirfs) OpenFile(name string, flag int, perm os.FileMode) (afero.File, error) {
	return c.backend.OpenFile(c.name(name), flag, perm)
}

func (c *Chdirfs) Remove(name string) error {
	return c.backend.Remove(c.name(name))
}

func (c *Chdirfs) RemoveAll(path string) error {
	return c.backend.RemoveAll(c.name(path))
}

func (c *Chdirfs) Rename(oldname string, newname string) error {
	return c.backend.Rename(c.name(oldname), c.name(newname))
}

func (c *Chdirfs) Stat(name string) (os.FileInfo, error) {
	return c.backend.Stat(c.name(name))
}

func (c *Chdirfs) Name() string {
	return "chdirfs"
}

func (c *Chdirfs) Chmod(name string, mode os.FileMode) error {
	return c.backend.Chmod(c.name(name), mode)
}

func (c *Chdirfs) Chtimes(name string, atime time.Time, mtime time.Time) error {
	return c.backend.Chtimes(c.name(name), atime, mtime)
}
