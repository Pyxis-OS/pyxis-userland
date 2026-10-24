// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
)

// Relative parents use the retained cwd chain on Pyxis, including after rename.
// Never reopen its descriptive Getwd spelling or seek another granted root.
func discoverRepository(invocationPath bool) (prefix string, err error) {
	origin, err := os.Open(".")
	if err != nil {
		return "", err
	}
	defer func() { err = errors.Join(err, origin.Close()) }()
	original, err := origin.Stat()
	if err != nil {
		return "", err
	}
	root := "."
	var names []string
	for {
		info, lookupErr := os.Lstat(filepath.Join(root, ".git"))
		if lookupErr == nil {
			if !info.IsDir() {
				return "", errors.New(".git must be a real directory; linked worktrees are unsupported")
			}
			slices.Reverse(names)
			prefix = filepath.Join(names...)
			if err := os.Chdir(root); err != nil {
				return "", err
			}
			if invocationPath {
				check := prefix
				if check == "" {
					check = "."
				}
				current, err := os.Lstat(check)
				if err != nil {
					return "", err
				}
				if !os.SameFile(original, current) {
					return "", errors.New("invocation directory changed during repository discovery")
				}
			}
			return prefix, nil
		}
		if !errors.Is(lookupErr, os.ErrNotExist) {
			return "", lookupErr
		}
		child, err := os.Open(root)
		if err != nil {
			return "", err
		}
		parentPath := filepath.Join(root, "..")
		parent, openErr := os.Open(parentPath)
		if openErr != nil {
			return "", errors.Join(fmt.Errorf("no repository within held cwd ancestors: %w", openErr), child.Close())
		}
		childInfo, childErr := child.Stat()
		parentInfo, parentErr := parent.Stat()
		if childErr != nil || parentErr != nil {
			return "", errors.Join(childErr, parentErr, child.Close(), parent.Close())
		}
		if os.SameFile(childInfo, parentInfo) {
			return "", errors.Join(errors.New("no repository within cwd ancestors"), child.Close(), parent.Close())
		}
		if invocationPath {
			name, err := retainedChildName(parent, childInfo)
			if err != nil {
				return "", errors.Join(err, child.Close(), parent.Close())
			}
			names = append(names, name)
		}
		if err := errors.Join(child.Close(), parent.Close()); err != nil {
			return "", err
		}
		root = parentPath
	}
}

func retainedChildName(parent *os.File, child os.FileInfo) (string, error) {
	entries, err := parent.ReadDir(-1)
	if err != nil {
		return "", err
	}
	name := ""
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		info, err := entry.Info()
		if err != nil {
			return "", err
		}
		if os.SameFile(child, info) {
			if name != "" {
				return "", errors.New("ambiguous invocation directory identity")
			}
			name = entry.Name()
		}
	}
	if name == "" {
		return "", errors.New("cannot identify invocation directory within retained parent")
	}
	return name, nil
}
