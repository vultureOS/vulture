package chdir

import (
	"errors"
	"path/filepath"

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
