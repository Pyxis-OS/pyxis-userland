// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"errors"
	"os"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/go-git/gcfg"
)

var errAppConfig = errors.New("invalid home://.config/git/config; expected unique [user] name/email and exact-origin [http] sslCAInfo settings")

// Both consumers parse the same literal application configuration. Parsing a
// CA path never opens it: commit only consumes identity; HTTPS only opens the
// CA file selected for its initial exact origin. No includes or expansion.
type appConfig struct {
	Name, Email string
	HTTPCA      map[string]string
}

func readAppConfig() (*appConfig, error) {
	cfg := &appConfig{HTTPCA: make(map[string]string)}
	path, err := identityPath()
	if err != nil {
		return nil, errors.New("cannot locate home://.config/git/config")
	}
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		return cfg, nil
	}
	if err != nil {
		return nil, errors.New("cannot read home://.config/git/config")
	}
	userSeen := false
	selected := ""
	err = gcfg.ReadWithCallback(f, func(section, subsection, key, value string, blank bool) error {
		isUser := strings.EqualFold(section, "user") && subsection == ""
		isHTTP := strings.EqualFold(section, "http") && subsection != ""
		if !isUser && !isHTTP {
			return errAppConfig
		}
		if key == "" {
			if isUser {
				if userSeen {
					return errAppConfig
				}
				userSeen = true
				return nil
			}
			u, err := validateHTTPSURL(subsection)
			if err != nil || u.Path != "" && u.Path != "/" || u.RawPath != "" {
				return errAppConfig
			}
			selected, err = httpsOrigin(u)
			if err != nil {
				return errAppConfig
			}
			if _, exists := cfg.HTTPCA[selected]; exists {
				return errAppConfig
			}
			cfg.HTTPCA[selected] = ""
			return nil
		}
		if blank || strings.TrimSpace(value) == "" || !utf8.ValidString(value) ||
			strings.ContainsFunc(value, unicode.IsControl) {
			return errAppConfig
		}
		if isUser {
			if !validIdentityField(value) {
				return errAppConfig
			}
			switch {
			case strings.EqualFold(key, "name") && cfg.Name == "":
				cfg.Name = value
			case strings.EqualFold(key, "email") && cfg.Email == "":
				cfg.Email = value
			default:
				return errAppConfig
			}
			return nil
		}
		if !strings.EqualFold(key, "sslCAInfo") || cfg.HTTPCA[selected] != "" {
			return errAppConfig
		}
		if _, _, err := rootedCAPath(value); err != nil {
			return errAppConfig
		}
		cfg.HTTPCA[selected] = value
		return nil
	})
	closeErr := f.Close()
	// The parser may quote arbitrary input. Never expose its diagnostics.
	if err != nil {
		return nil, errAppConfig
	}
	if closeErr != nil {
		return nil, errors.New("cannot close home://.config/git/config")
	}
	for _, ca := range cfg.HTTPCA {
		if ca == "" {
			return nil, errAppConfig
		}
	}
	return cfg, nil
}

func rootedCAPath(raw string) (string, string, error) {
	root, relative, ok := strings.Cut(raw, "://")
	if !ok || root == "" || relative == "" || !utf8.ValidString(raw) ||
		strings.ContainsFunc(raw, unicode.IsControl) ||
		strings.ContainsAny(root, "/\\:") || strings.TrimSpace(root) != root {
		return "", "", errors.New("sslCAInfo requires an explicit named-root path")
	}
	return root, relative, nil
}
