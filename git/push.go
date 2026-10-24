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
	"github.com/go-git/go-git/v5/plumbing/object"
	"github.com/go-git/go-git/v5/plumbing/transport"
)

func push(ctx context.Context, args []string) (err error) {
	setUpstream := len(args) == 3 && args[0] == "-u"
	if !setUpstream && len(args) > 1 {
		return errors.New("usage: git push [REMOTE] | git push -u REMOTE CURRENT_BRANCH")
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
	if _, err := object.GetCommit(s, branch.Hash()); err != nil {
		return fmt.Errorf("push requires a readable commit tip: %w", err)
	}
	cfg, err := s.Config()
	if err != nil {
		return err
	}
	branchName := head.Target().Short()
	upstream := cfg.Branches[branchName]
	name := ""
	if setUpstream {
		if args[2] != branchName {
			return errors.New("push -u only accepts the current branch's name")
		}
		name = args[1]
	} else {
		if upstream == nil || upstream.Remote == "" || upstream.Merge == "" {
			return fmt.Errorf("current branch %s has no upstream; to push and set it, use:\n    git push -u REMOTE %s", branchName, branchName)
		}
		if upstream.Merge != head.Target() {
			return errors.New("simple push requires the upstream branch to have the current branch's name")
		}
		name = upstream.Remote
		if len(args) == 1 && args[0] != name {
			return errors.New("simple push only targets the configured upstream remote")
		}
	}
	if err := safeRemoteName(name); err != nil {
		return err
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
	tracking := plumbing.NewRemoteReferenceName(name, branchName)
	if err := safeReferenceName(tracking); err != nil {
		return err
	}
	source := config.RefSpec(branch.Hash().String() + ":" + string(head.Target()))
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
	if setUpstream {
		if err := updateRepositoryConfig(s, func(latest *config.Config) error {
			currentHead, err := s.Reference(plumbing.HEAD)
			if err != nil {
				return err
			}
			if currentHead.Type() != plumbing.SymbolicReference || currentHead.Target() != head.Target() {
				return errors.New("current branch changed during push")
			}
			currentRemote := latest.Remotes[name]
			if currentRemote == nil || len(currentRemote.URLs) != 1 || currentRemote.URLs[0] != remote.URLs[0] {
				return errors.New("remote configuration changed during push")
			}
			currentUpstream := latest.Branches[branchName]
			if upstream == nil && currentUpstream != nil || upstream != nil &&
				(currentUpstream == nil || currentUpstream.Remote != upstream.Remote || currentUpstream.Merge != upstream.Merge) {
				return errors.New("upstream configuration changed during push")
			}
			if latest.Branches == nil {
				latest.Branches = make(map[string]*config.Branch)
			}
			if currentUpstream == nil {
				currentUpstream = &config.Branch{Name: branchName}
				latest.Branches[branchName] = currentUpstream
			}
			currentUpstream.Remote = name
			currentUpstream.Merge = head.Target()
			return nil
		}); err != nil {
			return fmt.Errorf("remote confirmed the push, but upstream configuration failed: %w", err)
		}
	}
	if err := s.Close(); err != nil {
		return fmt.Errorf("remote confirmed the push, but local close failed: %w", err)
	}
	if setUpstream {
		if _, err := fmt.Fprintf(os.Stdout, "Branch %s now tracks %s/%s\n", branchName, name, branchName); err != nil {
			return err
		}
	}
	if current {
		_, err = fmt.Fprintln(os.Stdout, "Already up to date")
	} else {
		_, err = fmt.Fprintf(os.Stdout, "Pushed %s to %s/%s\n", branch.Hash(), name, branchName)
	}
	return err
}
