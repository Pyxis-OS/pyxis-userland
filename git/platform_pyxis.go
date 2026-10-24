// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"syscall"
	"time"
)

func identityPath() (string, error) { return "home://.config/git/config", nil }

func wallTime() (time.Time, error) {
	ts, err := syscall.ClockWallNow()
	if err != nil {
		return time.Time{}, err
	}
	return time.Unix(ts.Sec, ts.Nsec), nil
}
