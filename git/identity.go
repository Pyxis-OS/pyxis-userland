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

	"github.com/go-git/go-git/v5/plumbing/object"
)

func commitSignature() (_ *object.Signature, err error) {
	cfg, err := readAppConfig()
	if err != nil {
		return nil, err
	}
	if cfg.Name == "" || cfg.Email == "" {
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
	return &object.Signature{Name: cfg.Name, Email: cfg.Email, When: when.In(zone)}, nil
}

func validIdentityField(value string) bool {
	return strings.TrimSpace(value) != "" && utf8.ValidString(value) &&
		!strings.ContainsAny(value, "<>") && !strings.ContainsFunc(value, unicode.IsControl)
}
