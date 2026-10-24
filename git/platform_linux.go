// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// The Linux reference uses a private HOME during qualification. Native Pyxis
// always resolves the held home capability, not a host path or XDG override.
func identityPath() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".config", "git", "config"), nil
}

func credentialsPath() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".config", "git", "credentials"), nil
}

// Only home:// has a Linux reference mapping. Other named roots remain a
// native authority choice and are refused here rather than guessed as paths.
func caFilePath(raw string) (string, error) {
	root, relative, err := rootedCAPath(raw)
	if err != nil {
		return "", err
	}
	if root != "home" {
		return "", errors.New("sslCAInfo named root is unavailable in the Linux reference")
	}
	clean := filepath.Clean(relative)
	if filepath.IsAbs(relative) || clean == ".." || strings.HasPrefix(clean, ".."+string(filepath.Separator)) {
		return "", errors.New("sslCAInfo path escapes its named root")
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return "", errors.New("sslCAInfo home root unavailable")
	}
	return filepath.Join(home, clean), nil
}

func wallTime() (time.Time, error) { return time.Now(), nil }
