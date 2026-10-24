// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path"
	"sort"
	"strings"

	git "github.com/go-git/go-git/v5"
	"github.com/go-git/go-git/v5/config"
	"github.com/go-git/go-git/v5/plumbing"
	"github.com/go-git/go-git/v5/plumbing/filemode"
	"github.com/go-git/go-git/v5/plumbing/format/index"
	"github.com/go-git/go-git/v5/plumbing/object"
	"github.com/go-git/go-git/v5/plumbing/storer"
	gitstorage "github.com/go-git/go-git/v5/storage"
)

// Pull deliberately does not use Worktree.Pull or Reset: those publish HEAD
// before checkout. Here all worktree writers and the index close successfully
// before the observed branch is compared and published. A failed checkout can
// leave partial worktree changes, but never authorizes a reset or early HEAD.
func pull(ctx context.Context, args []string) (err error) {
	remoteName := "origin"
	if len(args) > 1 || (len(args) == 1 && (args[0] == "" || strings.HasPrefix(args[0], "-"))) {
		return errors.New("usage: git pull [REMOTE]")
	}
	if len(args) == 1 {
		remoteName = args[0]
	}
	r, s, _, err := openRepository(true)
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, s.Close()) }()
	head, branch, err := pullHead(s)
	if err != nil {
		return err
	}
	cfg, err := r.Config()
	if err != nil {
		return err
	}
	remoteCfg := cfg.Remotes[remoteName]
	if remoteCfg == nil || len(remoteCfg.URLs) != 1 {
		return errors.New("pull requires a configured remote with exactly one HTTPS URL")
	}
	remoteBranch := head.Target()
	if upstream := cfg.Branches[head.Target().Short()]; upstream != nil && upstream.Remote == remoteName {
		remoteBranch = upstream.Merge
	}
	if err := safeBranchName(remoteBranch); err != nil {
		return errors.New("pull requires a valid upstream branch")
	}
	tracking := plumbing.NewRemoteReferenceName(remoteName, remoteBranch.Short())
	if err := safeReferenceName(tracking); err != nil {
		return errors.New("unsupported pull remote name")
	}
	before, err := pullTree(ctx, r, branch)
	if err != nil {
		return err
	}
	fs := &nativeFS{root: ".", writable: true}
	if err := pullClean(ctx, s, fs, before); err != nil {
		return err
	}
	session, err := newHTTPS(remoteCfg.URLs[0], false)
	if err != nil {
		return err
	}
	// Ignore configured fetch wildcards, tag behavior and other remote URLs.
	// A rewritten tracking ref only records observation; local HEAD remains
	// subject to the independent fast-forward check below.
	remote := git.NewRemote(s, &config.RemoteConfig{Name: remoteName, URLs: []string{session.URL}})
	fetchErr := remote.FetchContext(ctx, &git.FetchOptions{
		RemoteName: remoteName, RemoteURL: session.URL, Auth: session.Auth,
		RefSpecs: []config.RefSpec{config.RefSpec("+" + remoteBranch.String() + ":" + tracking.String())},
		Tags:     git.NoTags,
	})
	if fetchErr != nil && !errors.Is(fetchErr, git.NoErrAlreadyUpToDate) {
		return session.failure(fetchErr)
	}
	fetched, err := s.Reference(tracking)
	if err != nil || fetched.Type() != plumbing.HashReference || fetched.Hash().IsZero() {
		return errors.New("pull requires an existing remote branch tip")
	}
	tip := fetched.Hash()
	if branch != nil {
		ahead, err := pullAncestor(ctx, r, tip, branch.Hash())
		if err != nil {
			return err
		}
		if ahead {
			tip = branch.Hash()
		} else {
			ff, err := pullAncestor(ctx, r, branch.Hash(), tip)
			if err != nil {
				return err
			}
			if !ff {
				return errors.New("pull refuses divergent history; fetched tracking references may remain")
			}
		}
	}
	after, err := pullTree(ctx, r, plumbing.NewHashReference(head.Target(), tip))
	if err != nil {
		return err
	}
	if err := pullCollisions(ctx, fs, before, after); err != nil {
		return err
	}
	headLock, err := lockMetadata("HEAD.lock")
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, headLock.cleanup()) }()
	indexLock, err := lockMetadata("index.lock")
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, indexLock.cleanup()) }()
	if err := pullSameHead(s, head, branch); err != nil {
		return err
	}
	if err := pullClean(ctx, s, fs, before); err != nil {
		return err
	}
	if err := pullCollisions(ctx, fs, before, after); err != nil {
		return err
	}
	if branch == nil || tip != branch.Hash() {
		// From here onward preserve whatever partial state actually remains.
		defer func() {
			if err != nil {
				err = fmt.Errorf("pull failed after checkout began; partial worktree or index changes may remain, with no automatic reset: %w", err)
			}
		}()
		if err := pullCheckout(ctx, r, fs, before, after); err != nil {
			return err
		}
		idx := &index.Index{Version: 2}
		for _, name := range pullNames(after) {
			file := after[name]
			idx.Entries = append(idx.Entries, &index.Entry{Name: name, Mode: file.mode, Hash: file.hash})
		}
		if err := ctx.Err(); err != nil {
			return err
		}
		writer := bufio.NewWriter(indexLock.file)
		if err := errors.Join(index.NewEncoder(writer).Encode(idx), writer.Flush()); err != nil {
			return err
		}
		if err := indexLock.publish("index"); err != nil {
			return err
		}
		if err := ctx.Err(); err != nil {
			return err
		}
		if err := pullSameHead(s, head, branch); err != nil {
			return err
		}
		if err := publishBranch(s, plumbing.NewHashReference(head.Target(), tip), branch); err != nil {
			return err
		}
	}
	if err := errors.Join(s.Close(), indexLock.cleanup(), headLock.cleanup()); err != nil {
		return err
	}
	_, err = fmt.Fprintln(os.Stdout, tip)
	return err
}

func pullHead(s *profileStorage) (*plumbing.Reference, *plumbing.Reference, error) {
	head, err := s.Reference(plumbing.HEAD)
	if err != nil {
		return nil, nil, err
	}
	if head.Type() != plumbing.SymbolicReference || !head.Target().IsBranch() {
		return nil, nil, errors.New("pull requires a checked-out branch; detached HEAD is unsupported")
	}
	if err := safeBranchName(head.Target()); err != nil {
		return nil, nil, err
	}
	branch, err := s.Reference(head.Target())
	if errors.Is(err, plumbing.ErrReferenceNotFound) {
		return head, nil, nil
	}
	if err != nil {
		return nil, nil, err
	}
	if branch.Type() != plumbing.HashReference || branch.Hash().IsZero() {
		return nil, nil, errors.New("pull requires an existing or unborn branch")
	}
	return head, branch, nil
}

func pullSameHead(s *profileStorage, head, branch *plumbing.Reference) error {
	nowHead, nowBranch, err := pullHead(s)
	if err != nil {
		return err
	}
	if nowHead.Target() != head.Target() || (nowBranch == nil) != (branch == nil) ||
		(branch != nil && nowBranch.Hash() != branch.Hash()) {
		return gitstorage.ErrReferenceHasChanged
	}
	return nil
}

func pullTree(ctx context.Context, r *git.Repository, ref *plumbing.Reference) (map[string]*diffFile, error) {
	files := make(map[string]*diffFile)
	if ref == nil {
		return files, nil
	}
	commit, err := r.CommitObject(ref.Hash())
	if err != nil {
		return nil, err
	}
	tree, err := commit.Tree()
	if err != nil {
		return nil, err
	}
	if err := diffTreeFiles(ctx, r, tree, "", 0, files); err != nil {
		return nil, err
	}
	return files, nil
}

func pullAncestor(ctx context.Context, r *git.Repository, ancestor, tip plumbing.Hash) (bool, error) {
	commit, err := r.CommitObject(tip)
	if err != nil {
		return false, err
	}
	found := false
	iter := object.NewCommitPreorderIter(commit, nil, nil)
	defer iter.Close()
	err = iter.ForEach(func(c *object.Commit) error {
		if err := ctx.Err(); err != nil {
			return err
		}
		if c.Hash == ancestor {
			found = true
			return storer.ErrStop
		}
		return nil
	})
	return found, err
}

// Require exact index/tree agreement and hash every tracked ordinary file.
// Stat-cache timestamps and native mode bits cannot make a dirty file clean.
// Gitlinks remain opaque IDs; their held directory contents are never opened.
func pullClean(ctx context.Context, s *profileStorage, fs *nativeFS, files map[string]*diffFile) error {
	idx, err := s.Index()
	if err != nil {
		return err
	}
	if len(idx.Entries) != len(files) {
		return errors.New("pull requires a clean tracked tree and index")
	}
	seen := make(map[string]bool, len(idx.Entries))
	for _, e := range idx.Entries {
		if err := ctx.Err(); err != nil {
			return err
		}
		if e == nil {
			return errors.New("pull refuses a malformed index")
		}
		old := files[e.Name]
		if old == nil || seen[e.Name] || e.Stage != 0 || e.IntentToAdd || e.SkipWorktree ||
			e.Hash != old.hash || e.Mode != old.mode {
			return errors.New("pull requires a clean tracked tree and index")
		}
		seen[e.Name] = true
		if e.Mode == filemode.Submodule {
			continue
		}
		info, err := fs.Lstat(e.Name)
		if err != nil {
			return fmt.Errorf("pull requires a clean tracked file %q: %w", e.Name, err)
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("pull requires a clean ordinary tracked file: %q", e.Name)
		}
		f, err := fs.Open(e.Name)
		if err != nil {
			return err
		}
		hasher := plumbing.NewHasher(plumbing.BlobObject, info.Size())
		n, readErr := io.Copy(hasher, diffReader{ctx: ctx, reader: f})
		if err := errors.Join(readErr, f.Close()); err != nil {
			return err
		}
		if n != info.Size() || hasher.Sum() != e.Hash {
			return fmt.Errorf("pull requires a clean tracked file: %q", e.Name)
		}
	}
	return ctx.Err()
}

func pullNames(files map[string]*diffFile) []string {
	names := make([]string, 0, len(files))
	for name := range files {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

// Check every affected destination, including file/directory transitions.
// An existing directory can be replaced only when every descendant (including
// empty directories) belongs to the old tracked ordinary-file hierarchy.
func pullCollisions(ctx context.Context, fs *nativeFS, before, after map[string]*diffFile) error {
	trackedDirs := make(map[string]bool)
	for name, file := range before {
		if file.mode == filemode.Submodule {
			continue
		}
		for dir := path.Dir(name); dir != "."; dir = path.Dir(dir) {
			trackedDirs[dir] = true
		}
	}
	collision := func(name string) error { return fmt.Errorf("pull would overwrite an untracked path: %q", name) }
	var removableDir func(string) error
	removableDir = func(name string) error {
		if err := ctx.Err(); err != nil {
			return err
		}
		if !trackedDirs[name] {
			return collision(name)
		}
		entries, err := fs.ReadDir(name)
		if err != nil {
			return err
		}
		for _, entry := range entries {
			child := path.Join(name, entry.Name())
			if entry.IsDir() {
				if err := removableDir(child); err != nil {
					return err
				}
			} else if old := before[child]; old == nil || old.mode == filemode.Submodule || !entry.Mode().IsRegular() {
				return collision(child)
			}
		}
		return nil
	}
	for _, name := range pullNames(after) {
		if err := ctx.Err(); err != nil {
			return err
		}
		if after[name].mode == filemode.Submodule {
			if old := before[name]; old != nil {
				continue // existing tracked path; preserve any gitlink contents
			}
			info, err := fs.Lstat(name)
			if errors.Is(err, os.ErrNotExist) {
				continue
			}
			if err != nil {
				return err
			}
			if !info.IsDir() {
				return collision(name)
			}
			if err := removableDir(name); err != nil {
				return err
			}
			continue // only a checked old tracked hierarchy may become a gitlink
		}
		parts := strings.Split(name, "/")
		for i := 1; i <= len(parts); i++ {
			prefix := strings.Join(parts[:i], "/")
			info, err := fs.Lstat(prefix)
			if errors.Is(err, os.ErrNotExist) {
				break
			}
			if err != nil {
				return err
			}
			if info.IsDir() {
				if i == len(parts) {
					if err := removableDir(prefix); err != nil {
						return err
					}
				}
				continue
			}
			old := before[prefix]
			if old == nil || old.mode == filemode.Submodule || !info.Mode().IsRegular() {
				return collision(prefix)
			}
			// The checked old tracked file is removed before its new children
			// are created; do not stat through a file parent.
			break
		}
	}
	return ctx.Err()
}

func pullCheckout(ctx context.Context, r *git.Repository, fs *nativeFS, before, after map[string]*diffFile) error {
	for _, name := range pullNames(before) {
		if before[name].mode == filemode.Submodule {
			continue // never remove a gitlink's held contents
		}
		if next := after[name]; next != nil && next.mode != filemode.Submodule {
			continue
		}
		if err := ctx.Err(); err != nil {
			return err
		}
		if err := fs.Remove(name); err != nil {
			return err
		}
	}
	for _, name := range pullNames(after) {
		file := after[name]
		if file.mode == filemode.Submodule {
			continue
		}
		if old := before[name]; old != nil && old.mode == file.mode && old.hash == file.hash {
			continue
		}
		if err := ctx.Err(); err != nil {
			return err
		}
		info, err := fs.Lstat(name)
		if err == nil && info.IsDir() {
			if err := pullRemoveDirs(ctx, fs, name); err != nil {
				return err
			}
		} else if err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}
		if err := pullWriteFile(ctx, r, fs, file); err != nil {
			return fmt.Errorf("checkout %q: %w", name, err)
		}
	}
	return ctx.Err()
}

// Collision preflight proved that these directories held only tracked files,
// now removed. Never remove a file discovered after preflight.
func pullRemoveDirs(ctx context.Context, fs *nativeFS, name string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	entries, err := fs.ReadDir(name)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if !entry.IsDir() {
			return fmt.Errorf("directory changed during pull checkout: %q", name)
		}
		if err := pullRemoveDirs(ctx, fs, path.Join(name, entry.Name())); err != nil {
			return err
		}
	}
	return fs.Remove(name)
}

func pullWriteFile(ctx context.Context, r *git.Repository, fs *nativeFS, file *diffFile) (err error) {
	blob, err := r.BlobObject(file.hash)
	if err != nil {
		return err
	}
	source, err := blob.Reader()
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, source.Close()) }()
	f, err := fs.TempFile(path.Dir(file.name), ".git-pull-")
	if err != nil {
		return err
	}
	open, published := true, false
	defer func() {
		if open {
			err = errors.Join(err, f.Close())
		}
		if !published {
			err = errors.Join(err, fs.Remove(f.Name()))
		}
	}()
	hasher := plumbing.NewHasher(plumbing.BlobObject, blob.Size)
	n, err := io.Copy(io.MultiWriter(f, hasher), diffReader{ctx: ctx, reader: source})
	if err != nil {
		return err
	}
	if n != blob.Size {
		return io.ErrUnexpectedEOF
	}
	if hasher.Sum() != file.hash {
		return errors.New("checkout blob content does not match its object ID")
	}
	if err := f.Close(); err != nil {
		open = false
		return err
	}
	open = false
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := fs.Rename(f.Name(), file.name); err != nil {
		return err
	}
	published = true
	return nil
}
