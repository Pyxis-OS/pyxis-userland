// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"

	git "github.com/go-git/go-git/v5"
	"github.com/go-git/go-git/v5/plumbing"
	"github.com/go-git/go-git/v5/plumbing/filemode"
	fdiff "github.com/go-git/go-git/v5/plumbing/format/diff"
	"github.com/go-git/go-git/v5/plumbing/object"
	"github.com/go-git/go-git/v5/utils/binary"
	textdiff "github.com/go-git/go-git/v5/utils/diff"
	dmp "github.com/sergi/go-diff/diffmatchpatch"
)

// Compare index entries explicitly: repository/worktree diff convenience APIs
// can compare against HEAD and can omit reads based on cached stat information.
// Untracked files do not participate in a worktree-versus-index diff.
func showDiff(ctx context.Context, args []string) (err error) {
	cached := len(args) == 1 && args[0] == "--cached"
	if len(args) != 0 && !cached {
		return usage()
	}
	r, s, _, err := openRepository(false)
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, s.Close()) }()
	if err := ctx.Err(); err != nil {
		return err
	}
	idx, err := s.Index()
	if err != nil {
		return err
	}
	indexed := make(map[string]*diffFile, len(idx.Entries))
	for _, entry := range idx.Entries {
		if entry == nil {
			return errors.New("invalid nil index entry")
		}
		if entry.Stage != 0 || entry.IntentToAdd || entry.SkipWorktree {
			return fmt.Errorf("diff does not support conflicted, intent-to-add or sparse index entries: %q", entry.Name)
		}
		if err := indexPath(entry.Name); err != nil {
			return err
		}
		if !diffFileMode(entry.Mode) || entry.Hash.IsZero() {
			return fmt.Errorf("unsupported index entry: %q", entry.Name)
		}
		if _, exists := indexed[entry.Name]; exists {
			return fmt.Errorf("duplicate index entry: %q", entry.Name)
		}
		indexed[entry.Name] = &diffFile{name: entry.Name, mode: entry.Mode, hash: entry.Hash}
	}
	before := indexed
	if cached {
		before = make(map[string]*diffFile)
		head, err := r.Head()
		if err != nil {
			return err
		}
		commit, err := r.CommitObject(head.Hash())
		if err != nil {
			return err
		}
		tree, err := commit.Tree()
		if err != nil {
			return err
		}
		if err := diffTreeFiles(ctx, r, tree, "", 0, before); err != nil {
			return err
		}
	}
	paths := make(map[string]bool, len(before)+len(indexed))
	for name := range before {
		paths[name] = true
	}
	for name := range indexed {
		paths[name] = true
	}
	names := make([]string, 0, len(paths))
	for name := range paths {
		names = append(names, name)
	}
	sort.Strings(names)
	fs := &nativeFS{root: "."}
	encoder := fdiff.NewUnifiedEncoder(diffWriter{ctx: ctx, writer: os.Stdout}, 3)
	for _, name := range names {
		if err := ctx.Err(); err != nil {
			return err
		}
		from, to := before[name], indexed[name]
		var toContent []byte
		if !cached {
			// A gitlink is an opaque recorded commit. Do not inspect or
			// initialize its directory to find worktree submodule changes.
			if from.mode == filemode.Submodule {
				continue
			}
			to, toContent, err = diffWorktreeFile(ctx, fs, from)
			if err != nil {
				return fmt.Errorf("diff %q: %w", name, err)
			}
		}
		if from != nil && to != nil && from.hash == to.hash && from.mode == to.mode {
			continue
		}
		fromContent, err := diffObjectContent(ctx, r, from)
		if err != nil {
			return fmt.Errorf("diff %q: %w", name, err)
		}
		if cached {
			toContent, err = diffObjectContent(ctx, r, to)
			if err != nil {
				return fmt.Errorf("diff %q: %w", name, err)
			}
		}
		patch, err := diffMakePatch(ctx, from, to, fromContent, toContent)
		if err != nil {
			return err
		}
		if err := encoder.Encode(patch); err != nil {
			return err
		}
	}
	return ctx.Err()
}

func diffFileMode(mode filemode.FileMode) bool {
	switch mode {
	case filemode.Regular, filemode.Deprecated, filemode.Executable, filemode.Symlink, filemode.Submodule:
		return true
	}
	return false
}

// TreeWalker turns a missing child-tree error into EOF. Enumerate explicitly
// so a corrupt/inaccessible subtree can never produce a successful partial diff.
func diffTreeFiles(ctx context.Context, r *git.Repository, tree *object.Tree, prefix string, depth int, files map[string]*diffFile) error {
	if depth > 1024 {
		return object.ErrMaxTreeDepth
	}
	names := make(map[string]bool, len(tree.Entries))
	for _, entry := range tree.Entries {
		if err := ctx.Err(); err != nil {
			return err
		}
		if strings.Contains(entry.Name, "/") || names[entry.Name] {
			return fmt.Errorf("invalid or duplicate tree entry: %q", prefix+entry.Name)
		}
		names[entry.Name] = true
		name := prefix + entry.Name
		if err := indexPath(name); err != nil {
			return err
		}
		if entry.Hash.IsZero() {
			return fmt.Errorf("invalid tree entry: %q", name)
		}
		if entry.Mode == filemode.Dir {
			child, err := r.TreeObject(entry.Hash)
			if err != nil {
				return fmt.Errorf("read tree %q: %w", name, err)
			}
			if err := diffTreeFiles(ctx, r, child, name+"/", depth+1, files); err != nil {
				return err
			}
			continue
		}
		if !diffFileMode(entry.Mode) {
			return fmt.Errorf("unsupported tree mode for %q", name)
		}
		files[name] = &diffFile{name: name, mode: entry.Mode, hash: entry.Hash}
	}
	return nil
}

func diffWorktreeFile(ctx context.Context, fs *nativeFS, from *diffFile) (*diffFile, []byte, error) {
	// A tracked parent replaced with a file removes its tracked descendants.
	// Inspect ancestors separately so ENOTDIR cannot hide other routing errors.
	parts := strings.Split(from.name, "/")
	for i := 1; i <= len(parts); i++ {
		info, err := fs.Lstat(strings.Join(parts[:i], "/"))
		if errors.Is(err, os.ErrNotExist) {
			return nil, nil, nil
		}
		if err != nil {
			return nil, nil, err
		}
		if i < len(parts) {
			if !info.IsDir() {
				return nil, nil, nil
			}
		} else if info.IsDir() {
			return nil, nil, nil
		} else if !info.Mode().IsRegular() {
			return nil, nil, fmt.Errorf("unsupported worktree file kind: %s", info.Mode())
		}
	}
	f, err := fs.Open(from.name)
	if err != nil {
		return nil, nil, err
	}
	content, readErr := io.ReadAll(diffReader{ctx: ctx, reader: f})
	if err := errors.Join(readErr, f.Close()); err != nil {
		return nil, nil, err
	}
	// core.fileMode=false and core.symlinks=false preserve the original Git
	// mode while reading executable/symlink-target text as an ordinary file.
	return &diffFile{name: from.name, mode: from.mode, hash: plumbing.ComputeHash(plumbing.BlobObject, content)}, content, nil
}

func diffObjectContent(ctx context.Context, r *git.Repository, file *diffFile) ([]byte, error) {
	if file == nil {
		return nil, nil
	}
	if file.mode == filemode.Submodule {
		return []byte("Subproject commit " + file.hash.String() + "\n"), nil
	}
	blob, err := r.BlobObject(file.hash)
	if err != nil {
		return nil, err
	}
	reader, err := blob.Reader()
	if err != nil {
		return nil, err
	}
	content, readErr := io.ReadAll(diffReader{ctx: ctx, reader: reader})
	return content, errors.Join(readErr, reader.Close())
}

type diffFile struct {
	name string
	mode filemode.FileMode
	hash plumbing.Hash
}

func (f *diffFile) Path() string            { return f.name }
func (f *diffFile) Mode() filemode.FileMode { return f.mode }
func (f *diffFile) Hash() plumbing.Hash     { return f.hash }

type diffPatch struct {
	from, to *diffFile
	binary   bool
	chunks   []fdiff.Chunk
}

func (p *diffPatch) Message() string                { return "" }
func (p *diffPatch) FilePatches() []fdiff.FilePatch { return []fdiff.FilePatch{p} }
func (p *diffPatch) IsBinary() bool                 { return p.binary }
func (p *diffPatch) Chunks() []fdiff.Chunk          { return p.chunks }
func (p *diffPatch) Files() (from, to fdiff.File) {
	if p.from != nil {
		from = p.from
	}
	if p.to != nil {
		to = p.to
	}
	return
}

func diffMakePatch(ctx context.Context, from, to *diffFile, before, after []byte) (*diffPatch, error) {
	p := &diffPatch{from: from, to: to}
	for _, content := range [][]byte{before, after} {
		isBinary, err := binary.IsBinary(bytes.NewReader(content))
		if err != nil {
			return nil, err
		}
		p.binary = p.binary || isBinary
	}
	if p.binary {
		return p, ctx.Err()
	}
	for _, chunk := range textdiff.Do(string(before), string(after)) {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		op := fdiff.Equal
		switch chunk.Type {
		case dmp.DiffInsert:
			op = fdiff.Add
		case dmp.DiffDelete:
			op = fdiff.Delete
		}
		p.chunks = append(p.chunks, diffChunk{content: chunk.Text, operation: op})
	}
	return p, ctx.Err()
}

type diffChunk struct {
	content   string
	operation fdiff.Operation
}

func (c diffChunk) Content() string       { return c.content }
func (c diffChunk) Type() fdiff.Operation { return c.operation }

type diffReader struct {
	ctx    context.Context
	reader io.Reader
}

func (r diffReader) Read(p []byte) (int, error) {
	if err := r.ctx.Err(); err != nil {
		return 0, err
	}
	return r.reader.Read(p)
}

type diffWriter struct {
	ctx    context.Context
	writer io.Writer
}

func (w diffWriter) Write(p []byte) (int, error) {
	if err := w.ctx.Err(); err != nil {
		return 0, err
	}
	n, err := w.writer.Write(p)
	if err == nil && n != len(p) {
		err = io.ErrShortWrite
	}
	return n, err
}
