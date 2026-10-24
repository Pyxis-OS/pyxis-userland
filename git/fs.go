// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"unicode/utf8"

	"github.com/go-git/go-billy/v5"
)

// Paths remain relative to the retained process cwd. No operation changes cwd
// after library work starts, or reopens its descriptive Getwd spelling.
// This confines library routing, not other roots already held by the process.
type nativeFS struct {
	root     string
	writable bool
}

func (fs *nativeFS) path(name string) (string, error) {
	if filepath.IsAbs(name) || strings.Contains(name, "://") || !utf8.ValidString(name) || strings.ContainsRune(name, 0) {
		return "", fmt.Errorf("unsupported repository path %q", name)
	}
	for _, part := range strings.Split(name, string(filepath.Separator)) {
		if part == ".." || len(part) > 255 {
			return "", fmt.Errorf("unsupported repository component %q", part)
		}
	}
	path := filepath.Join(fs.root, name)
	// Native resolution cannot follow symlinks. Keep the Linux reference
	// profile from following existing symlinked metadata or worktree parents.
	if runtime.GOOS == "linux" {
		prefix := "."
		for _, part := range strings.Split(path, string(filepath.Separator)) {
			prefix = filepath.Join(prefix, part)
			info, err := os.Lstat(prefix)
			if errors.Is(err, os.ErrNotExist) {
				break
			}
			if err != nil {
				return "", err
			}
			if info.Mode()&os.ModeSymlink != 0 {
				return "", fmt.Errorf("symlink routing is unsupported: %s", prefix)
			}
		}
	}
	return path, nil
}

func (fs *nativeFS) Capabilities() billy.Capability {
	capabilities := billy.ReadCapability | billy.SeekCapability
	if fs.writable {
		capabilities |= billy.WriteCapability | billy.ReadAndWriteCapability | billy.TruncateCapability
	}
	return capabilities
}

func (fs *nativeFS) Root() string { return fs.root }

func (fs *nativeFS) Join(parts ...string) string { return filepath.Join(parts...) }

func (fs *nativeFS) Chroot(name string) (billy.Filesystem, error) {
	path, err := fs.path(name)
	if err != nil {
		return nil, err
	}
	return &nativeFS{root: path, writable: fs.writable}, nil
}

func (fs *nativeFS) Create(name string) (billy.File, error) {
	return fs.OpenFile(name, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0666)
}

func (fs *nativeFS) Open(name string) (billy.File, error) {
	return fs.OpenFile(name, os.O_RDONLY, 0)
}

func (fs *nativeFS) OpenFile(name string, flags int, mode os.FileMode) (billy.File, error) {
	path, err := fs.path(name)
	if err != nil {
		return nil, err
	}
	writable := flags&(os.O_WRONLY|os.O_RDWR|os.O_CREATE|os.O_TRUNC|os.O_APPEND) != 0
	if writable && !fs.writable {
		return nil, billy.ErrReadOnly
	}
	if flags&os.O_CREATE != 0 {
		if err := os.MkdirAll(filepath.Dir(path), 0777); err != nil {
			return nil, err
		}
	}
	f, err := os.OpenFile(path, flags, mode)
	if err != nil {
		return nil, err
	}
	return &nativeFile{File: f, name: name, dirty: flags&(os.O_CREATE|os.O_TRUNC) != 0}, nil
}

func (fs *nativeFS) TempFile(dir, prefix string) (billy.File, error) {
	if !fs.writable {
		return nil, billy.ErrReadOnly
	}
	path, err := fs.path(dir)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(path, 0777); err != nil {
		return nil, err
	}
	f, err := os.CreateTemp(path, prefix)
	if err != nil {
		return nil, err
	}
	name, err := filepath.Rel(fs.root, f.Name())
	if err != nil {
		return nil, errors.Join(err, f.Close())
	}
	return &nativeFile{File: f, name: name, dirty: true}, nil
}

func (fs *nativeFS) Stat(name string) (os.FileInfo, error) {
	return fs.Lstat(name)
}

func (fs *nativeFS) Lstat(name string) (os.FileInfo, error) {
	path, err := fs.path(name)
	if err != nil {
		return nil, err
	}
	return os.Lstat(path)
}

func (fs *nativeFS) ReadDir(name string) ([]os.FileInfo, error) {
	path, err := fs.path(name)
	if err != nil {
		return nil, err
	}
	entries, err := os.ReadDir(path)
	if err != nil {
		return nil, err
	}
	infos := make([]os.FileInfo, 0, len(entries))
	for _, entry := range entries {
		info, err := entry.Info()
		if err != nil {
			return nil, err
		}
		infos = append(infos, info)
	}
	return infos, nil
}

func (fs *nativeFS) MkdirAll(name string, mode os.FileMode) error {
	if !fs.writable {
		return billy.ErrReadOnly
	}
	path, err := fs.path(name)
	if err != nil {
		return err
	}
	return os.MkdirAll(path, mode)
}

func (fs *nativeFS) Rename(oldName, newName string) error {
	if !fs.writable {
		return billy.ErrReadOnly
	}
	oldPath, err := fs.path(oldName)
	if err != nil {
		return err
	}
	newPath, err := fs.path(newName)
	if err != nil {
		return err
	}
	info, err := os.Lstat(oldPath)
	if err != nil {
		return err
	}
	if info.Mode().IsRegular() {
		// Billy can publish a still-open writer or one already closed. Sync a
		// real WRITE handle in either case, before changing its final name.
		f, err := os.OpenFile(oldPath, os.O_WRONLY, 0)
		if err != nil {
			return err
		}
		if err := errors.Join(f.Sync(), f.Close()); err != nil {
			return err
		}
	} else if !info.IsDir() {
		return fmt.Errorf("cannot publish unsupported file kind: %s", oldName)
	}
	if err := os.MkdirAll(filepath.Dir(newPath), 0777); err != nil {
		return err
	}
	return os.Rename(oldPath, newPath)
}

func (fs *nativeFS) Remove(name string) error {
	if !fs.writable {
		return billy.ErrReadOnly
	}
	path, err := fs.path(name)
	if err != nil {
		return err
	}
	return os.Remove(path)
}

func (fs *nativeFS) Symlink(target, link string) error { return billy.ErrNotSupported }

func (fs *nativeFS) Readlink(name string) (string, error) { return "", billy.ErrNotSupported }

type nativeFile struct {
	*os.File
	name  string
	dirty bool
}

func (f *nativeFile) Name() string { return f.name }

func (f *nativeFile) Write(p []byte) (int, error) {
	n, err := f.File.Write(p)
	if n != 0 {
		f.dirty = true
	}
	return n, err
}

func (f *nativeFile) Truncate(size int64) error {
	f.dirty = true
	return f.File.Truncate(size)
}

func (f *nativeFile) Close() error {
	var err error
	if f.dirty {
		err = f.File.Sync()
	}
	return errors.Join(err, f.File.Close())
}

func (f *nativeFile) Lock() error { return billy.ErrNotSupported }

func (f *nativeFile) Unlock() error { return billy.ErrNotSupported }
