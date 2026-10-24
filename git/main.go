// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"path"
	"strconv"
	"strings"
	"unicode/utf8"

	git "github.com/go-git/go-git/v5"
	"github.com/go-git/go-git/v5/config"
	"github.com/go-git/go-git/v5/plumbing/cache"
	"github.com/go-git/go-git/v5/plumbing/transport/client"
	githttp "github.com/go-git/go-git/v5/plumbing/transport/http"
	"github.com/go-git/go-git/v5/storage/filesystem"
)

func main() {
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt)
	defer cancel()
	if err := run(ctx, os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "git:", err)
		os.Exit(1)
	}
}

func usage() error {
	return errors.New("usage: git clone HTTPS_URL [DIRECTORY] | git log [-n COUNT] | git status")
}

func run(ctx context.Context, args []string) error {
	if len(args) == 0 {
		return usage()
	}
	switch args[0] {
	case "clone":
		return clone(ctx, args[1:])
	case "log":
		count := -1
		if len(args) != 1 {
			if len(args) != 3 || args[1] != "-n" {
				return usage()
			}
			var err error
			count, err = strconv.Atoi(args[2])
			if err != nil || count < 1 {
				return errors.New("log count must be a positive integer")
			}
		}
		return showLog(ctx, count)
	case "status":
		if len(args) != 1 {
			return usage()
		}
		return showStatus(ctx)
	default:
		return usage()
	}
}

// The accepted checkout profile applies in memory even for a read-only open.
// Reading log/status never rewrites an existing repository's configuration.
type profileStorage struct {
	*filesystem.Storage
	closed bool
}

func (s *profileStorage) Close() error {
	if s.closed {
		return nil
	}
	s.closed = true
	return s.Storage.Close()
}

func (s *profileStorage) Config() (*config.Config, error) {
	cfg, err := s.Storage.Config()
	if err != nil {
		return nil, err
	}
	if cfg.Core.IsBare || cfg.Core.Worktree != "" {
		return nil, errors.New("bare repositories and redirected worktrees are unsupported")
	}
	for _, section := range cfg.Raw.Sections {
		if strings.EqualFold(section.Name, "include") || strings.EqualFold(section.Name, "includeIf") {
			return nil, errors.New("configuration includes are unsupported")
		}
	}
	cfg.Core.Symlinks = config.OptBoolFalse
	cfg.Core.FileMode = config.OptBoolFalse
	return cfg, nil
}

func storage(writable bool) *profileStorage {
	return &profileStorage{Storage: filesystem.NewStorage(&nativeFS{root: ".git", writable: writable}, cache.NewObjectLRUDefault())}
}

func openRepository() (*git.Repository, *profileStorage, error) {
	info, err := os.Lstat(".git")
	if err != nil {
		return nil, nil, fmt.Errorf("run from the repository root: %w", err)
	}
	if !info.IsDir() {
		return nil, nil, errors.New(".git must be a real directory; linked worktrees are unsupported")
	}
	for _, name := range []string{".git/commondir", ".git/objects/info/alternates", ".git/objects/info/http-alternates"} {
		if _, err := os.Lstat(name); err == nil {
			return nil, nil, fmt.Errorf("unsupported repository indirection: %s", name)
		} else if !errors.Is(err, os.ErrNotExist) {
			return nil, nil, err
		}
	}
	s := storage(false)
	r, err := git.Open(s, &nativeFS{root: "."})
	if err != nil {
		return nil, nil, errors.Join(err, s.Close())
	}
	return r, s, nil
}

func clone(ctx context.Context, args []string) (err error) {
	if len(args) < 1 || len(args) > 2 {
		return usage()
	}
	u, err := url.Parse(args[0])
	if err != nil {
		return err
	}
	if u.Scheme != "https" || u.Hostname() == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" {
		return errors.New("clone requires a public HTTPS URL without credentials, query or fragment")
	}
	dir := strings.TrimSuffix(path.Base(strings.TrimSuffix(u.Path, "/")), ".git")
	if len(args) == 2 {
		dir = args[1]
	}
	if dir == "" || dir == "." || dir == ".." || len(dir) > 255 || !utf8.ValidString(dir) || strings.ContainsAny(dir, "/\x00") {
		return errors.New("clone destination must be one fresh child directory name")
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := os.Mkdir(dir, 0777); err != nil {
		return err
	}
	defer func() {
		if err != nil {
			err = fmt.Errorf("%w; incomplete clone retained in %q", err, dir)
		}
	}()
	if err := os.Chdir(dir); err != nil {
		return err
	}
	httpClient := &http.Client{CheckRedirect: func(request *http.Request, via []*http.Request) error {
		if len(via) >= 10 {
			return errors.New("too many HTTPS redirects")
		}
		if request.URL.Scheme != "https" || request.URL.User != nil {
			return errors.New("refusing non-HTTPS or credential-bearing redirect")
		}
		return nil
	}}
	client.InstallProtocol("https", githttp.NewClient(httpClient))
	s := storage(true)
	defer func() { err = errors.Join(err, s.Close()) }()
	r, err := git.CloneContext(ctx, s, &nativeFS{root: ".", writable: true}, &git.CloneOptions{URL: u.String(), NoCheckout: true})
	if err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	cfg, err := r.Config()
	if err != nil {
		return err
	}
	if err := r.SetConfig(cfg); err != nil {
		return err
	}
	w, err := r.Worktree()
	if err != nil {
		return err
	}
	if err := w.Reset(&git.ResetOptions{Mode: git.HardReset}); err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	head, err := r.Head()
	if err != nil {
		return err
	}
	if err := s.Close(); err != nil {
		return err
	}
	_, err = fmt.Fprintf(os.Stdout, "Cloned %s at %s\n", dir, head.Hash())
	return err
}

func showLog(ctx context.Context, count int) (err error) {
	r, s, err := openRepository()
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, s.Close()) }()
	commits, err := r.Log(&git.LogOptions{})
	if err != nil {
		return err
	}
	defer commits.Close()
	for count != 0 {
		if err := ctx.Err(); err != nil {
			return err
		}
		commit, err := commits.Next()
		if errors.Is(err, io.EOF) {
			return nil
		}
		if err != nil {
			return err
		}
		if _, err := fmt.Fprint(os.Stdout, commit.String()); err != nil {
			return err
		}
		if count > 0 {
			count--
		}
	}
	return nil
}

func showStatus(ctx context.Context) (err error) {
	r, s, err := openRepository()
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, s.Close()) }()
	w, err := r.Worktree()
	if err != nil {
		return err
	}
	status, err := w.Status()
	if err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := s.Close(); err != nil {
		return err
	}
	if status.IsClean() {
		_, err = fmt.Fprintln(os.Stdout, "working tree clean")
	} else {
		_, err = fmt.Fprint(os.Stdout, status.String())
	}
	return err
}
