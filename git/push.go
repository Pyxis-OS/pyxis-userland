// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"context"
	"errors"
	"fmt"
	"os"

	git "github.com/go-git/go-git/v5"
	"github.com/go-git/go-git/v5/config"
	"github.com/go-git/go-git/v5/plumbing"
	"github.com/go-git/go-git/v5/plumbing/transport"
)

func push(ctx context.Context, args []string) (err error) {
	if len(args) > 1 {
		return errors.New("usage: git push [REMOTE]")
	}
	_, s, _, err := openRepository(true)
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, s.Close()) }()
	head, err := s.Reference(plumbing.HEAD)
	if err != nil {
		return err
	}
	if head.Type() != plumbing.SymbolicReference {
		return errors.New("push requires a checked-out branch")
	}
	if err := safeBranchName(head.Target()); err != nil {
		return err
	}
	branch, err := s.Reference(head.Target())
	if err != nil {
		return fmt.Errorf("push requires a local commit: %w", err)
	}
	if branch.Type() != plumbing.HashReference || branch.Hash().IsZero() {
		return errors.New("push requires a valid branch tip")
	}
	cfg, err := s.Config()
	if err != nil {
		return err
	}
	upstream := cfg.Branches[head.Target().Short()]
	if upstream == nil || upstream.Remote == "" || upstream.Merge == "" {
		return errors.New("current branch has no configured upstream; refusing simple push")
	}
	if err := safeRemoteName(upstream.Remote); err != nil {
		return err
	}
	if upstream.Merge != head.Target() {
		return errors.New("simple push requires the upstream branch to have the current branch's name")
	}
	name := upstream.Remote
	if len(args) == 1 && args[0] != name {
		return errors.New("simple push only targets the configured upstream remote")
	}
	remote := cfg.Remotes[name]
	if remote == nil || len(remote.URLs) != 1 {
		return errors.New("upstream requires a configured remote with one HTTPS URL")
	}
	session, err := newHTTPS(remote.URLs[0], true)
	if err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	// Freeze the selected commit and send exactly one destination branch.
	// Never allow go-git's implicit all-branches refspec or configured defaults.
	tracking := plumbing.NewRemoteReferenceName(name, upstream.Merge.Short())
	if err := safeReferenceName(tracking); err != nil {
		return err
	}
	source := config.RefSpec(branch.Hash().String() + ":" + string(upstream.Merge))
	// Suppress configured post-push fetch mappings: only this tracking ref
	// may change locally, and its failure must not hide a confirmed push.
	selected := &config.RemoteConfig{Name: name, URLs: []string{session.URL}}
	err = git.NewRemote(s, selected).PushContext(ctx, &git.PushOptions{
		RemoteName: name,
		RemoteURL:  session.URL,
		RefSpecs:   []config.RefSpec{source},
		Auth:       session.Auth,
	})
	current := errors.Is(err, git.NoErrAlreadyUpToDate)
	if err != nil && !current {
		if errors.Is(err, git.ErrNonFastForwardUpdate) {
			return errors.New("push refused: non-fast-forward update; no remote update was sent")
		}
		safe := session.failure(err)
		if errors.Is(err, transport.ErrAuthenticationRequired) || errors.Is(err, transport.ErrAuthorizationFailed) {
			return safe
		}
		return fmt.Errorf("%w; remote state may have changed, inspect it before retrying", safe)
	}
	if err := s.SetReference(plumbing.NewHashReference(tracking, branch.Hash())); err != nil {
		return fmt.Errorf("remote confirmed the push, but local tracking update failed: %w", err)
	}
	if err := s.Close(); err != nil {
		return fmt.Errorf("remote confirmed the push, but local close failed: %w", err)
	}
	if current {
		_, err = fmt.Fprintln(os.Stdout, "Already up to date")
	} else {
		_, err = fmt.Fprintf(os.Stdout, "Pushed %s to %s/%s\n", branch.Hash(), name, upstream.Merge.Short())
	}
	return err
}
