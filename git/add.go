// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"strings"
	"unicode/utf8"

	"github.com/go-git/go-billy/v5"
	git "github.com/go-git/go-git/v5"
	"github.com/go-git/go-git/v5/plumbing/filemode"
	"github.com/go-git/go-git/v5/plumbing/format/gitignore"
	"github.com/go-git/go-git/v5/plumbing/format/index"
)

// Staging uses the library's per-file path validation and object writer, but
// keeps every index update private until the entire requested scan succeeds.
// The real index is never opened for truncation.
type stagingStorage struct {
	*profileStorage
	idx *index.Index
}

func (s *stagingStorage) Index() (*index.Index, error) { return s.idx, nil }
func (s *stagingStorage) SetIndex(idx *index.Index) error {
	s.idx = idx
	return nil
}

// The lock name is owned only after O_EXCL succeeds. Close (including native
// Sync) and cleanup errors are always checked, including on a failed scan.
type metadataLock struct {
	fs        *nativeFS
	file      billy.File
	name      string
	open      bool
	published bool
}

func lockMetadata(name string) (*metadataLock, error) {
	fs := &nativeFS{root: ".git", writable: true}
	f, err := fs.OpenFile(name, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0666)
	if err != nil {
		return nil, fmt.Errorf("acquire %s (busy locks are not retried): %w", name, err)
	}
	return &metadataLock{fs: fs, file: f, name: name, open: true}, nil
}

func (l *metadataLock) close() error {
	if !l.open {
		return nil
	}
	l.open = false
	return l.file.Close()
}

func (l *metadataLock) cleanup() error {
	err := l.close()
	if !l.published {
		err = errors.Join(err, l.fs.Remove(l.name))
		l.published = true
	}
	if err != nil {
		return fmt.Errorf("release %s: %w", l.name, err)
	}
	return nil
}

func (l *metadataLock) publish(name string) error {
	if err := l.close(); err != nil {
		return err
	}
	if err := l.fs.Rename(l.name, name); err != nil {
		return err
	}
	l.published = true
	return nil
}

func addOperands(args []string, prefix string) ([]string, error) {
	if len(args) == 1 && args[0] == "-A" {
		return []string{"."}, nil
	}
	literal := len(args) > 0 && args[0] == "--"
	if literal {
		args = args[1:]
	}
	if len(args) == 0 {
		return nil, errors.New("usage: git add [--] PATH... | git add -A")
	}
	operands := make([]string, 0, len(args))
	for _, operand := range args {
		if operand == "" || (!literal && strings.HasPrefix(operand, "-")) ||
			filepath.IsAbs(operand) || strings.Contains(operand, "://") ||
			strings.ContainsRune(operand, 0) || !utf8.ValidString(operand) {
			return nil, fmt.Errorf("unsupported literal add operand %q", operand)
		}
		name := path.Clean(path.Join(prefix, operand))
		if name == ".." || strings.HasPrefix(name, "../") {
			return nil, fmt.Errorf("add operand leaves the repository: %q", operand)
		}
		if name != "." {
			if err := indexPath(name); err != nil {
				return nil, err
			}
		}
		operands = append(operands, name)
	}
	return operands, nil
}

// Keep index paths canonical before using them for selection or tree building.
// New paths additionally pass go-git's ValidTreePath through AddWithOptions.
func indexPath(name string) error {
	if name == "" || name == "." || path.Clean(name) != name ||
		filepath.IsAbs(name) || strings.Contains(name, "://") || !utf8.ValidString(name) {
		return fmt.Errorf("unsupported index path %q", name)
	}
	for _, part := range strings.Split(name, "/") {
		if part == ".." || len(part) > 255 || strings.EqualFold(part, ".git") || strings.EqualFold(part, "git~1") {
			return fmt.Errorf("unsupported index path %q", name)
		}
		for _, c := range part {
			if c < 0x20 || c == 0x7f {
				return fmt.Errorf("unsupported index path %q", name)
			}
		}
	}
	return nil
}

func withinOperand(name, operand string) bool {
	return operand == "." || name == operand || strings.HasPrefix(name, operand+"/")
}

func stage(ctx context.Context, args []string) (err error) {
	r, s, prefix, err := openRepository(true)
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, s.Close()) }()
	operands, err := addOperands(args, prefix)
	if err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	lock, err := lockMetadata("index.lock")
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, lock.cleanup()) }()
	idx, err := s.Index()
	if err != nil {
		return err
	}
	original := append([]*index.Entry(nil), idx.Entries...)
	tracked := make(map[string]*index.Entry, len(original))
	stages := make(map[string]uint, len(original))
	for _, e := range original {
		if e == nil {
			return errors.New("malformed nil index entry")
		}
		if err := indexPath(e.Name); err != nil {
			return err
		}
		if e.SkipWorktree || e.IntentToAdd || e.Mode.IsMalformed() || e.Mode == filemode.Dir || e.Hash.IsZero() || e.Stage < 0 || e.Stage > 3 {
			return fmt.Errorf("unsupported index entry: %s", e.Name)
		}
		bit := uint(1) << uint(e.Stage)
		if stages[e.Name]&bit != 0 || (e.Stage == 0 && stages[e.Name] != 0) || stages[e.Name]&1 != 0 {
			return fmt.Errorf("duplicate or mixed conflict stages: %s", e.Name)
		}
		stages[e.Name] |= bit
		// On conflict resolution retain the checked-out (ours) mode when
		// available; all conflict stages for the selected file are replaced.
		if previous := tracked[e.Name]; previous == nil || e.Stage == 2 {
			tracked[e.Name] = e
		}
	}
	private := &stagingStorage{profileStorage: s, idx: idx}
	r, err = git.Open(private, &nativeFS{root: ".", writable: true})
	if err != nil {
		return err
	}
	w, err := r.Worktree()
	if err != nil {
		return err
	}
	patterns, err := gitignore.ReadPatterns(w.Filesystem, nil)
	if err != nil {
		return err
	}
	matcher := gitignore.NewMatcher(patterns)
	seen := make(map[string]bool)
	var scan func(string, bool) error
	scan = func(name string, explicit bool) error {
		if err := ctx.Err(); err != nil {
			return err
		}
		if seen[name] {
			return nil
		}
		for parent, e := range tracked {
			if e.Mode == filemode.Submodule && withinOperand(name, parent) {
				// Preserve recorded gitlinks, including absent worktrees.
				seen[parent] = true
				return nil
			}
		}
		fi, statErr := w.Filesystem.Lstat(name)
		if statErr != nil {
			if !errors.Is(statErr, os.ErrNotExist) {
				return statErr
			}
			for existing := range tracked {
				if withinOperand(existing, name) {
					return nil // selected tracked deletion; removed below
				}
			}
			return fmt.Errorf("add operand does not exist and is not tracked: %s", name)
		}
		if fi.IsDir() {
			if name != "." && matcher.Match(strings.Split(name, "/"), true) {
				hasTracked := false
				for existing := range tracked {
					if withinOperand(existing, name) {
						hasTracked = true
						break
					}
				}
				if !hasTracked {
					if explicit {
						return fmt.Errorf("path is ignored: %s", name)
					}
					return nil
				}
			}
			entries, err := w.Filesystem.ReadDir(name)
			if err != nil {
				return err
			}
			for _, entry := range entries {
				if strings.EqualFold(entry.Name(), ".git") {
					continue
				}
				if err := scan(path.Join(name, entry.Name()), false); err != nil {
					return err
				}
			}
			return nil
		}
		if !fi.Mode().IsRegular() {
			return fmt.Errorf("unsupported working-tree file kind: %s", name)
		}
		old := tracked[name]
		if old == nil && matcher.Match(strings.Split(name, "/"), false) {
			if explicit {
				return fmt.Errorf("path is ignored: %s", name)
			}
			return nil
		}
		if err := indexPath(name); err != nil {
			return err
		}
		// Remove conflict stages and file/directory collisions privately.
		kept := idx.Entries[:0]
		for _, e := range idx.Entries {
			if e.Name == name || strings.HasPrefix(e.Name, name+"/") || strings.HasPrefix(name, e.Name+"/") {
				if e.Mode == filemode.Submodule {
					return fmt.Errorf("cannot replace a gitlink: %s", e.Name)
				}
				continue
			}
			kept = append(kept, e)
		}
		idx.Entries = kept
		// Always hash selected files, including same-size/native-zero-mtime
		// edits. Status's stat cache must never decide what add records.
		if err := w.AddWithOptions(&git.AddOptions{Path: name, SkipStatus: true}); err != nil {
			return err
		}
		e, err := idx.Entry(name)
		if err != nil {
			return err
		}
		e.Mode = filemode.Regular
		if old != nil {
			e.Mode = old.Mode
		}
		seen[name] = true
		return nil
	}
	for _, operand := range operands {
		if err := scan(operand, true); err != nil {
			return err
		}
	}
	kept := idx.Entries[:0]
	for _, e := range idx.Entries {
		remove := false
		if e.Mode != filemode.Submodule && !seen[e.Name] {
			for _, operand := range operands {
				if withinOperand(e.Name, operand) {
					remove = true
					break
				}
			}
		}
		if !remove {
			kept = append(kept, e)
		}
	}
	idx.Entries = kept
	idx.Cache, idx.ResolveUndo, idx.EndOfIndexEntry = nil, nil, nil
	if err := ctx.Err(); err != nil {
		return err
	}
	writer := bufio.NewWriter(lock.file)
	if err := errors.Join(index.NewEncoder(writer).Encode(idx), writer.Flush()); err != nil {
		return err
	}
	if err := lock.publish("index"); err != nil {
		return err
	}
	return s.Close()
}
