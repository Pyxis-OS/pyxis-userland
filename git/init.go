// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
	"unicode/utf8"

	git "github.com/go-git/go-git/v5"
	"github.com/go-git/go-git/v5/config"
	"github.com/go-git/go-git/v5/plumbing"
	gitstorage "github.com/go-git/go-git/v5/storage"
)

func initialize(ctx context.Context, args []string) (err error) {
	if len(args) != 0 {
		return errors.New("usage: git init (current directory only)")
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	// Exclusive creation refuses files, directories and dangling symlinks.
	// Retain this directory on every later failure, including checked close.
	if err := os.Mkdir(".git", 0777); err != nil {
		return fmt.Errorf("create fresh .git: %w", err)
	}
	defer func() {
		if err != nil {
			err = fmt.Errorf("%w; partial initialization retained in .git", err)
		}
	}()
	s := storage(true)
	defer func() { err = errors.Join(err, s.Close()) }()
	if _, err := git.InitWithOptions(s, &nativeFS{root: ".", writable: true}, git.InitOptions{
		DefaultBranch: plumbing.NewBranchReferenceName("main"),
	}); err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := updateRepositoryConfig(s, func(cfg *config.Config) error {
		// These options describe the native working-tree projection. They do
		// not change the executable/symlink modes stored in Git objects.
		cfg.Raw.Section("core").SetOption("filemode", "false")
		cfg.Raw.Section("core").SetOption("symlinks", "false")
		return ctx.Err()
	}); err != nil {
		return err
	}
	if err := errors.Join(ctx.Err(), s.Close()); err != nil {
		return err
	}
	_, err = fmt.Fprintln(os.Stdout, "Initialized empty Git repository in .git (main)")
	return err
}

// A symbolic branch HEAD whose target is absent is unborn. A missing HEAD,
// invalid route or malformed existing reference remains an error.
func isUnborn(s *profileStorage) (bool, error) {
	head, err := s.Reference(plumbing.HEAD)
	if err != nil {
		return false, err
	}
	if head.Type() != plumbing.SymbolicReference {
		return false, nil
	}
	if err := safeBranchName(head.Target()); err != nil {
		return false, err
	}
	ref, err := s.Reference(head.Target())
	if errors.Is(err, plumbing.ErrReferenceNotFound) {
		return true, nil
	}
	if err != nil {
		return false, err
	}
	if ref.Type() != plumbing.HashReference || ref.Hash().IsZero() {
		return false, errors.New("invalid branch tip")
	}
	return false, nil
}

func safeBranchName(name plumbing.ReferenceName) error {
	if !name.IsBranch() {
		return errors.New("HEAD must name a branch")
	}
	return safeReferenceName(name)
}

// Validate every component before it is used as a metadata or lock route.
func safeReferenceName(name plumbing.ReferenceName) error {
	if err := name.Validate(); err != nil {
		return err
	}
	if !utf8.ValidString(string(name)) {
		return errors.New("reference name must be UTF-8")
	}
	for _, part := range strings.Split(string(name), "/") {
		if len(part) > 255 || strings.EqualFold(part, ".git") ||
			strings.EqualFold(part, "git~1") || strings.HasSuffix(part, ".lock") {
			return fmt.Errorf("unsupported reference component %q", part)
		}
	}
	return nil
}

// A nil old reference means create only if absent, rather than the library's
// unconditional SetReference semantics. Check absence while owning the same
// lock used by cooperating branch writers, then publish the checked bytes.
func publishBranch(s *profileStorage, ref, old *plumbing.Reference) (err error) {
	if ref == nil || ref.Type() != plumbing.HashReference || ref.Hash().IsZero() {
		return errors.New("branch publication requires a nonzero commit reference")
	}
	if err := safeBranchName(ref.Name()); err != nil {
		return err
	}
	if old != nil {
		if old.Name() != ref.Name() || old.Type() != plumbing.HashReference || old.Hash().IsZero() {
			return gitstorage.ErrReferenceHasChanged
		}
		return s.CheckAndSetReference(ref, old)
	}
	lock, err := lockMetadata(ref.Name().String() + ".lock")
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, lock.cleanup()) }()
	if _, err := s.Reference(ref.Name()); err == nil {
		return gitstorage.ErrReferenceHasChanged
	} else if !errors.Is(err, plumbing.ErrReferenceNotFound) {
		return err
	}
	data := ref.Hash().String() + "\n"
	n, err := io.WriteString(lock.file, data)
	if err != nil {
		return err
	}
	if n != len(data) {
		return io.ErrShortWrite
	}
	return lock.publish(ref.Name().String())
}
