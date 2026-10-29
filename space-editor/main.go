// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Command spaces edits the spaces in an installed system's boot.lua. Changes
// apply at the next boot.
package main

import (
	"fmt"
	"os"
)

const defaultPath = "system://config/boot.lua"

func main() {
	path := defaultPath
	switch {
	case len(os.Args) == 2 && (os.Args[1] == "-h" || os.Args[1] == "--help"):
		fmt.Println("usage: spaces [PATH]\n\nEdits the spaces in PATH, by default " + defaultPath + ".\nChanges apply at the next boot.")
		return
	case len(os.Args) == 2:
		path = os.Args[1]
	case len(os.Args) > 2:
		fmt.Fprintln(os.Stderr, "usage: spaces [PATH]")
		os.Exit(2)
	}
	if err := run(path); err != nil {
		fmt.Fprintln(os.Stderr, "spaces:", err)
		os.Exit(1)
	}
}

func run(path string) error {
	data, err := load(path)
	if err != nil {
		return loadFailure(path, err)
	}
	top, err := parseLiteral(string(data))
	if err != nil {
		return loadFailure(path, err)
	}
	cfg, err := decodeConfig(top)
	if err != nil {
		return loadFailure(path, err)
	}
	return newEditor(path, data, cfg, writable(path)).run()
}
