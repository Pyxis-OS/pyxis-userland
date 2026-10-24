// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"errors"
	"fmt"
	"os"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"

	format "github.com/go-git/go-git/v5/plumbing/format/config"
	"github.com/go-git/go-git/v5/plumbing/object"
)

func commitSignature() (_ *object.Signature, err error) {
	path, err := identityPath()
	if err != nil {
		return nil, err
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("read author identity %s: %w", path, err)
	}
	cfg := format.New()
	err = errors.Join(format.NewDecoder(f).Decode(cfg), f.Close())
	if err != nil {
		return nil, fmt.Errorf("author identity %s: %w", path, err)
	}
	name, email := "", ""
	for _, section := range cfg.Sections {
		if !section.IsName("user") || len(section.Subsections) != 0 {
			return nil, errors.New("author configuration permits only [user] name and email")
		}
		for _, option := range section.Options {
			switch {
			case option.IsKey("name") && name == "":
				name = option.Value
			case option.IsKey("email") && email == "":
				email = option.Value
			default:
				return nil, errors.New("unsupported or duplicate author configuration setting")
			}
			if !validIdentityField(option.Value) {
				return nil, errors.New("author name and email must be nonempty text without controls or angle brackets")
			}
		}
	}
	if name == "" || email == "" {
		return nil, errors.New("set [user] name and email in home://.config/git/config before committing")
	}
	when, err := wallTime()
	if err != nil {
		return nil, fmt.Errorf("commit wall time unavailable: %w", err)
	}
	zone := time.UTC
	if tz := os.Getenv("TZ"); tz != "" {
		if tz == "Local" {
			return nil, errors.New("commit timezone must name a configured IANA zone or UTC")
		}
		zone, err = time.LoadLocation(tz)
		if err != nil {
			return nil, fmt.Errorf("commit timezone %q: %w", tz, err)
		}
	}
	return &object.Signature{Name: name, Email: email, When: when.In(zone)}, nil
}

func validIdentityField(value string) bool {
	return strings.TrimSpace(value) != "" && utf8.ValidString(value) &&
		!strings.ContainsAny(value, "<>") && !strings.ContainsFunc(value, unicode.IsControl)
}
