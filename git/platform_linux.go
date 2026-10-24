// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"os"
	"path/filepath"
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

func wallTime() (time.Time, error) { return time.Now(), nil }
