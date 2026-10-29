// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

// Saving replaces boot.lua and its one backup with staged, synced files.
// Close and rename alone do not make data durable on Pyxis
// (docs/devices/explicit-durability.md), so each file is synced before its
// replacing rename and the directory after both.

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

var errChanged = errors.New("The file changed since it was opened. Reload it with Ctrl+R before saving.")

func previousPath(path string) string { return path + ".previous" }

// load reads the whole file, bounded by the literal source limit.
func load(path string) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, literalSourceLimit+1))
	if err != nil {
		return nil, err
	}
	if len(data) > literalSourceLimit {
		return nil, fmt.Errorf("larger than %d bytes", literalSourceLimit)
	}
	return data, nil
}

// writable reports why PATH cannot be replaced, without changing it.
func writable(path string) error {
	file, err := os.OpenFile(path, os.O_WRONLY, 0)
	if err != nil {
		return err
	}
	return file.Close()
}

// stage writes DATA beside TARGET, syncs it and renames it over TARGET.
func stage(target string, data []byte) error {
	staged := target + ".new"
	file, err := os.OpenFile(staged, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	if _, err := file.Write(data); err != nil {
		file.Close()
		return err
	}
	if err := file.Sync(); err != nil {
		file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	return os.Rename(staged, target)
}

func syncDirectory(path string) error {
	directory, err := os.Open(filepath.Dir(path))
	if err != nil {
		return err
	}
	defer directory.Close()
	return directory.Sync()
}

// saveConfig keeps LOADED as the backup and writes TEXT, refusing if the file
// no longer holds LOADED. A directory sync failure is returned after the
// rename, so the caller can say the change is in place but not yet durable.
func saveConfig(path string, loaded []byte, text string) (durable error, err error) {
	current, err := load(path)
	if err != nil {
		return nil, err
	}
	if !bytes.Equal(current, loaded) {
		return nil, errChanged
	}
	if err := stage(previousPath(path), loaded); err != nil {
		return nil, fmt.Errorf("writing the backup: %w", err)
	}
	if err := stage(path, []byte(text)); err != nil {
		return nil, err
	}
	return syncDirectory(path), nil
}
