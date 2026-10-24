// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path"
	"sort"
	"strings"

	git "github.com/go-git/go-git/v5"
	"github.com/go-git/go-git/v5/plumbing"
	"github.com/go-git/go-git/v5/plumbing/filemode"
	"github.com/go-git/go-git/v5/plumbing/format/index"
	"github.com/go-git/go-git/v5/plumbing/object"
	gitstorage "github.com/go-git/go-git/v5/storage"
)

// The upstream commit builder uses SetReference and rereads Index. Freeze the
// validated index and turn that final publication into a compare-and-swap of
// the observed branch. HEAD.lock remains held from observation through CAS,
// preventing a cooperating checkout from redirecting the commit to a branch
// other than the one whose tip is the explicit parent.
type commitStorage struct {
	*profileStorage
	ctx    context.Context
	idx    *index.Index
	head   *plumbing.Reference
	branch *plumbing.Reference
}

func (s *commitStorage) Index() (*index.Index, error) { return s.idx, nil }

func (s *commitStorage) SetReference(ref *plumbing.Reference) error {
	if err := s.ctx.Err(); err != nil {
		return err
	}
	if ref.Type() != plumbing.HashReference || ref.Name() != s.head.Target() {
		return gitstorage.ErrReferenceHasChanged
	}
	head, err := s.profileStorage.Reference(plumbing.HEAD)
	if err != nil {
		return err
	}
	if head.Type() != plumbing.SymbolicReference || head.Target() != s.head.Target() {
		return gitstorage.ErrReferenceHasChanged
	}
	return publishBranch(s.profileStorage, ref, s.branch)
}

func createCommit(ctx context.Context, args []string) (err error) {
	if len(args) != 2 || args[0] != "-m" || strings.TrimSpace(args[1]) == "" || strings.ContainsRune(args[1], 0) {
		return errors.New("usage: git commit -m MESSAGE (one nonempty message)")
	}
	signature, err := commitSignature()
	if err != nil {
		return err
	}
	_, s, _, err := openRepository(true)
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, s.Close()) }()
	if err := ctx.Err(); err != nil {
		return err
	}
	lock, err := lockMetadata("HEAD.lock")
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, lock.cleanup()) }()
	head, err := s.Reference(plumbing.HEAD)
	if err != nil {
		return err
	}
	if head.Type() != plumbing.SymbolicReference || !head.Target().IsBranch() {
		return errors.New("commit requires a checked-out branch; detached HEAD is unsupported")
	}
	if err := safeBranchName(head.Target()); err != nil {
		return err
	}
	branch, err := s.Reference(head.Target())
	if errors.Is(err, plumbing.ErrReferenceNotFound) {
		branch = nil
	} else if err != nil {
		return err
	}
	if branch != nil && (branch.Type() != plumbing.HashReference || branch.Hash().IsZero()) {
		return errors.New("commit requires an existing branch tip")
	}
	idx, err := s.Index()
	if err != nil {
		return err
	}
	names := make(map[string]bool, len(idx.Entries))
	for _, e := range idx.Entries {
		if e == nil {
			return errors.New("malformed nil index entry")
		}
		if e.Stage != 0 {
			return fmt.Errorf("unresolved index conflict: %s", e.Name)
		}
		if err := indexPath(e.Name); err != nil {
			return err
		}
		if e.SkipWorktree || e.IntentToAdd || e.Mode.IsMalformed() || e.Mode == filemode.Dir || e.Hash.IsZero() {
			return fmt.Errorf("unsupported index entry: %s", e.Name)
		}
		if names[e.Name] {
			return fmt.Errorf("duplicate index entry: %s", e.Name)
		}
		names[e.Name] = true
		// Gitlink commits need not exist in this repository's object store.
		if e.Mode != filemode.Submodule {
			if _, err := s.EncodedObject(plumbing.BlobObject, e.Hash); err != nil {
				return fmt.Errorf("indexed blob %s: %w", e.Name, err)
			}
		}
	}
	for name := range names {
		for prefix := path.Dir(name); prefix != "."; prefix = path.Dir(prefix) {
			if names[prefix] {
				return fmt.Errorf("index has both a file and descendants: %s", prefix)
			}
		}
	}
	private := &commitStorage{profileStorage: s, ctx: ctx, idx: idx, head: head, branch: branch}
	var hash plumbing.Hash
	if branch == nil {
		// Build the initial commit explicitly. CommitOptions.Validate may
		// discover a parent on a second HEAD read; the first commit always
		// has no parent, and branch creation checks absence under its lock.
		if len(idx.Entries) == 0 {
			return git.ErrEmptyCommit
		}
		tree, err := initialCommitTree(ctx, s, idx)
		if err != nil {
			return err
		}
		commit := &object.Commit{Author: *signature, Committer: *signature, Message: args[1], TreeHash: tree}
		encoded := s.NewEncodedObject()
		if err := commit.Encode(encoded); err != nil {
			return err
		}
		hash, err = s.SetEncodedObject(encoded)
		if err != nil {
			return err
		}
		if err := private.SetReference(plumbing.NewHashReference(head.Target(), hash)); err != nil {
			return err
		}
	} else {
		r, err := git.Open(private, &nativeFS{root: "."})
		if err != nil {
			return err
		}
		w, err := r.Worktree()
		if err != nil {
			return err
		}
		// Both signatures and the parent are explicit: Validate never searches
		// system/global identity or substitutes a library-generated timestamp.
		hash, err = w.Commit(args[1], &git.CommitOptions{
			Author: signature, Committer: signature,
			Parents: []plumbing.Hash{branch.Hash()},
		})
		if err != nil {
			return err
		}
	}
	if err := errors.Join(s.Close(), lock.cleanup()); err != nil {
		return err
	}
	_, err = fmt.Fprintln(os.Stdout, hash)
	return err
}

// Index blobs and modes are already validated. Build only tree objects; no
// working-tree reads or mode inference participate in an initial commit.
func initialCommitTree(ctx context.Context, s *profileStorage, idx *index.Index) (plumbing.Hash, error) {
	trees := map[string]*object.Tree{"": {}}
	for _, entry := range idx.Entries {
		dir := path.Dir(entry.Name)
		if dir == "." {
			dir = ""
		}
		for current := dir; current != ""; {
			if _, exists := trees[current]; exists {
				break
			}
			trees[current] = &object.Tree{}
			current = path.Dir(current)
			if current == "." {
				current = ""
			}
		}
		trees[dir].Entries = append(trees[dir].Entries, object.TreeEntry{
			Name: path.Base(entry.Name), Mode: entry.Mode, Hash: entry.Hash,
		})
	}
	names := make([]string, 0, len(trees))
	for name := range trees {
		names = append(names, name)
	}
	// Every child path is longer than its parent. Publish child trees first,
	// attaching their content-addressed entries before encoding the parent.
	sort.Slice(names, func(i, j int) bool { return len(names[i]) > len(names[j]) })
	for _, name := range names {
		if err := ctx.Err(); err != nil {
			return plumbing.ZeroHash, err
		}
		tree := trees[name]
		sort.Sort(object.TreeEntrySorter(tree.Entries))
		encoded := s.NewEncodedObject()
		if err := tree.Encode(encoded); err != nil {
			return plumbing.ZeroHash, err
		}
		hash, err := s.SetEncodedObject(encoded)
		if err != nil {
			return plumbing.ZeroHash, err
		}
		if name == "" {
			return hash, nil
		}
		parent := path.Dir(name)
		if parent == "." {
			parent = ""
		}
		trees[parent].Entries = append(trees[parent].Entries, object.TreeEntry{
			Name: path.Base(name), Mode: filemode.Dir, Hash: hash,
		})
	}
	return plumbing.ZeroHash, errors.New("initial commit tree has no root")
}
