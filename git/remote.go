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

	"github.com/go-git/go-git/v5/config"
	"github.com/go-git/go-git/v5/plumbing"
	format "github.com/go-git/go-git/v5/plumbing/format/config"
)

// Hold config.lock across the entire local read/modify/encode/publication.
// nativeFile.Close checks Sync; publish checks close before renaming the lock.
func updateRepositoryConfig(s *profileStorage, change func(*config.Config) error) (err error) {
	lock, err := lockMetadata("config.lock")
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, lock.cleanup()) }()
	cfg, err := s.Config()
	if err != nil {
		return err
	}
	if err := change(cfg); err != nil {
		return err
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	data, err := cfg.Marshal()
	if err != nil {
		return err
	}
	n, err := lock.file.Write(data)
	if err != nil {
		return err
	}
	if n != len(data) {
		return io.ErrShortWrite
	}
	return lock.publish("config")
}

func manageRemote(ctx context.Context, args []string) (err error) {
	list := len(args) == 1 && args[0] == "-v"
	if !list && (len(args) != 3 || (args[0] != "add" && args[0] != "set-url")) {
		return errors.New("usage: git remote add NAME HTTPS_URL | git remote set-url NAME HTTPS_URL | git remote -v")
	}
	var address string
	if !list {
		if err := safeRemoteName(args[1]); err != nil {
			return err
		}
		u, err := validateHTTPSURL(args[2])
		if err != nil {
			return err
		}
		address = u.String()
	}
	_, s, _, err := openRepository(!list)
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, s.Close()) }()
	if err := ctx.Err(); err != nil {
		return err
	}
	if !list {
		if err := updateRepositoryConfig(s, func(cfg *config.Config) error {
			_, exists := cfg.Remotes[args[1]]
			if args[0] == "add" && exists {
				return fmt.Errorf("remote %q already exists", args[1])
			}
			if args[0] == "set-url" && !exists {
				return fmt.Errorf("remote %q does not exist", args[1])
			}
			sub := cfg.Raw.Section("remote").Subsection(args[1])
			sub.SetOption("url", address)
			// The profile has one URL for both fetch and push. Remove an
			// old pushurl too, rather than leaving a hidden second target.
			sub.RemoveOption("pushurl")
			if !exists {
				sub.SetOption("fetch", fmt.Sprintf(config.DefaultFetchRefSpec, args[1]))
			}
			// Reparse the edited raw config so the library's private remote
			// fields cannot restore old URLs when Marshal runs. Unknown
			// options and all unrelated sections remain in the raw document.
			var raw bytes.Buffer
			if err := format.NewEncoder(&raw).Encode(cfg.Raw); err != nil {
				return err
			}
			updated, err := config.ReadConfig(&raw)
			if err != nil {
				return err
			}
			*cfg = *updated
			return ctx.Err()
		}); err != nil {
			return err
		}
		return s.Close()
	}
	cfg, err := s.Config()
	if err != nil {
		return err
	}
	names := make([]string, 0, len(cfg.Remotes))
	for name := range cfg.Remotes {
		names = append(names, name)
	}
	sort.Strings(names)
	var output strings.Builder
	for _, name := range names {
		if err := safeRemoteName(name); err != nil {
			return err
		}
		remote := cfg.Remotes[name]
		if len(remote.URLs) != 1 {
			return fmt.Errorf("remote %q requires exactly one HTTPS URL", name)
		}
		u, err := validateHTTPSURL(remote.URLs[0])
		if err != nil {
			return err
		}
		fmt.Fprintf(&output, "%s\t%s (fetch)\n%s\t%s (push)\n", name, u.String(), name, u.String())
	}
	if err := errors.Join(ctx.Err(), s.Close()); err != nil {
		return err
	}
	_, err = fmt.Fprint(os.Stdout, output.String())
	return err
}

func safeRemoteName(name string) error {
	if name == "" || strings.HasPrefix(name, "-") {
		return errors.New("remote name must be nonempty and not start with '-'")
	}
	return safeReferenceName(plumbing.ReferenceName("refs/remotes/" + name + "/x"))
}
