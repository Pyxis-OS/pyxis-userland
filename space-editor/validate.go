// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

// A copy of the space rules in userspace/libconfig/boot.c (read_space and
// boot_plan_build). It exists only because Go on Pyxis cannot launch
// `config validate` today; when it can, run that instead and remove this.
// Boot remains the authority. Top-level settings the editor carries through
// unchanged (hostname, input, wifi_policy, volume selectors) are not checked.

import (
	"fmt"
	"os"
	"runtime"
	"strings"
)

const (
	nameMax           = 31  // SPACE_NAME_MAX
	titleMax          = 63  // SPACE_TITLE_MAX
	initMax           = 255 // BOOT_INIT_MAX
	cpuIndexMax       = 4095
	sourceLimit       = 8 // BUNDLE_SOURCE_LIMIT
	sourcePathMax     = 255
	sourceComponents  = 16
	systemVolume      = "system"
	packagesVolume    = "packages"
	httpWorkVolume    = "http-work"
	httpStagingSource = "http_staging"
)

var volumeKinds = map[string]bool{
	"npfs": true, "ram-npfs": true, "virtio-fs": true, "ram": true, "boot-esp": true, "fat32": true,
}

// reservedSources are the startup names boot init gives selected sources itself.
var reservedSources = map[string]bool{
	"fat_mounts": true, "package_management": true, "package_inspect": true,
	"package_config": true, "package_config_write": true,
}

type severity int

const (
	errorProblem severity = iota
	warningProblem
)

// problem names the space (or -1) and the detail tab that fixes it.
type problem struct {
	severity severity
	space    int
	tab      int
	text     string
}

const (
	generalTab = iota
	settingsTab
	rootsTab
	grantsTab
)

func validName(name string) bool {
	if name == "" || len(name) > nameMax {
		return false
	}
	for i := 0; i < len(name); i++ {
		c := name[i]
		if !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-' {
			return false
		}
	}
	return true
}

func printable(text string) bool {
	for i := 0; i < len(text); i++ {
		if text[i] < 0x20 || text[i] > 0x7e {
			return false
		}
	}
	return true
}

func validSourceName(name string) bool {
	if name == "" || len(name) > nameMax || reservedSources[name] {
		return false
	}
	for i := 0; i < len(name); i++ {
		c := name[i]
		if !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-' && c != '_' {
			return false
		}
	}
	return true
}

func validSourcePath(path string) bool {
	parts := strings.Split(path, "/")
	if len(parts) > sourceComponents {
		return false
	}
	for _, part := range parts {
		if part == "" || part == "." || part == ".." {
			return false
		}
	}
	return true
}

func reservedVolume(name string) bool { return name == "boot" || name == "tmp" || name == "bin" }

func npfsBacked(v *volume) bool { return v != nil && (v.kind == "npfs" || v.kind == "ram-npfs") }

func fatBacked(v *volume) bool { return v != nil && (v.kind == "fat32" || v.kind == "boot-esp") }

func sameBacking(a, b *volume) bool {
	return a == b || (a.kind == b.kind && npfsBacked(a) && a.partition == b.partition && a.store == b.store)
}

// privateHTTPBacking mirrors private_http_backing: http-work stays outside
// every client root and source other than http_staging.
func (c *config) privateHTTPBacking(backing *volume) bool {
	system, packages := c.volume(systemVolume), c.volume(packagesVolume)
	if (system != nil && sameBacking(backing, system)) || (packages != nil && sameBacking(backing, packages)) {
		return false
	}
	if backing.kind == "npfs" && system != nil && system.kind == "npfs" &&
		backing.partition == system.partition && backing.store == "bin" {
		return false
	}
	for _, s := range c.spaces {
		for _, r := range s.roots {
			if v := c.volume(r.volumeName()); v != nil && sameBacking(backing, v) {
				return false
			}
		}
		for _, g := range s.grants {
			if v := c.volume(g.volume); v != nil && g.name != httpStagingSource && sameBacking(backing, v) {
				return false
			}
		}
	}
	return true
}

func (r *root) volumeName() string {
	if r.view != "" {
		return packagesVolume
	}
	return r.name
}

func (c *config) validate() []problem {
	var problems []problem
	add := func(sev severity, index, tab int, format string, arguments ...any) {
		problems = append(problems, problem{sev, index, tab, fmt.Sprintf(format, arguments...)})
	}

	for i := range c.volumes {
		v := &c.volumes[i]
		if !validName(v.name) || reservedVolume(v.name) {
			add(errorProblem, -1, 0, "volume %s: names use a-z, 0-9 and '-', other than boot, tmp and bin", v.name)
		}
		if !volumeKinds[v.kind] {
			add(errorProblem, -1, 0, "volume %s: kind must be npfs, ram-npfs, virtio-fs, ram, boot-esp or fat32", v.name)
		}
		if v.kind == "ram-npfs" && c.top.field("ram_npfs_bytes") == nil {
			add(errorProblem, -1, 0, "volume %s requires archive ram_npfs_bytes", v.name)
		}
		if v.name == httpWorkVolume {
			if v.kind != "ram" && !npfsBacked(v) {
				add(errorProblem, -1, 0, "volume %s requires private RAM or NPFS backing", v.name)
			} else if !c.privateHTTPBacking(v) {
				add(errorProblem, -1, 0, "volume %s must remain outside all client roots and sources", v.name)
			}
		}
	}

	focus, network := "", ""
	names := map[string]bool{}
	for i, s := range c.spaces {
		label := "space " + s.name
		if !validName(s.name) || s.name == "rescue" || s.name == "caelum" {
			add(errorProblem, i, generalTab, "%s: names use 1 to %d of a-z, 0-9 and '-', other than rescue and caelum", label, nameMax)
		} else if names[s.name] {
			add(errorProblem, i, generalTab, "space %s is configured twice", s.name)
		}
		names[s.name] = true
		if s.title != "" && (len(s.title) > titleMax || !printable(s.title)) {
			add(errorProblem, i, generalTab, "%s: title must be 1 to %d printable characters", label, titleMax)
		}
		if len(s.init) <= len("boot://") || len(s.init) > initMax || !printable(s.init) ||
			(!strings.HasPrefix(s.init, "boot://") && !strings.HasPrefix(s.init, "system://")) {
			add(errorProblem, i, generalTab, "%s: init must name a boot:// archive entry or a system:// file", label)
		} else if runtime.GOOS == "pyxis" && strings.HasPrefix(s.init, "system://") {
			if _, err := os.Stat(s.init); err != nil {
				add(warningProblem, i, generalTab, "%s: init %s is not readable here; boot would leave the space unstarted", label, s.init)
			}
		}
		if s.cpus != nil {
			if len(s.cpus) == 0 {
				add(errorProblem, i, generalTab, "%s: cpus must not be empty", label)
			}
			seen := map[int64]bool{}
			for _, cpu := range s.cpus {
				if cpu < 0 || cpu > cpuIndexMax {
					add(errorProblem, i, generalTab, "%s: CPU index %d must be from 0 to %d", label, cpu, cpuIndexMax)
				} else if seen[cpu] {
					add(errorProblem, i, generalTab, "%s: CPU %d is listed twice", label, cpu)
				}
				seen[cpu] = true
			}
		}
		if s.start != "" {
			if !validName(s.start) {
				add(errorProblem, i, generalTab, "%s: start must name a root", label)
			} else if !reservedVolume(s.start) && s.root(s.start) == nil {
				add(errorProblem, i, generalTab, "%s starts in a root it does not have", label)
			}
		}
		if s.flags["focus"] {
			if focus != "" {
				add(errorProblem, i, settingsTab, "more than one space sets focus = true, including %s and %s", focus, s.name)
			} else {
				focus = s.name
			}
		}
		if s.flags["network"] {
			if network != "" {
				add(errorProblem, i, settingsTab, "more than one space sets network = true, including %s", s.name)
			} else {
				network = s.name
			}
		}

		packages := c.volume(packagesVolume)
		if s.flags["package_management"] && !npfsBacked(packages) {
			add(errorProblem, i, settingsTab, "%s: package_management needs an npfs packages volume", label)
		}
		sources := len(s.grants)
		if s.fat32Mount != "" {
			sources++
		}
		if sources > sourceLimit {
			add(errorProblem, i, grantsTab, "%s: too many selected sources (%d, at most %d)", label, sources, sourceLimit)
		} else {
			if packages != nil {
				sources++
			}
			if c.volume(systemVolume) != nil {
				sources++
			}
			if s.flags["package_management"] {
				sources += 2
			}
			if s.flags["package_upgrade"] {
				sources += 2
			}
			if sources > sourceLimit {
				add(errorProblem, i, grantsTab, "%s: too many selected sources including packages (%d, at most %d)", label, sources, sourceLimit)
			}
		}

		for _, r := range s.roots {
			what := label + ": root " + r.name
			if !validName(r.name) || reservedVolume(r.name) || r.name == "acpi" || r.name == "jit" {
				add(errorProblem, i, rootsTab, "%s: roots must name configured volumes or views", what)
				continue
			}
			if r.view != "" && r.view != "pkg" && r.view != "sdk" {
				add(errorProblem, i, rootsTab, "%s: view must be \"pkg\" or \"sdk\"", what)
				continue
			}
			if (r.view == "pkg") != (r.name == "pkg") || (r.view == "sdk") != (r.name == "sdk") {
				add(errorProblem, i, rootsTab, "%s: pkg and sdk name only their view roots", what)
				continue
			}
			v := c.volume(r.volumeName())
			switch {
			case v == nil:
				add(errorProblem, i, rootsTab, "%s names undefined volume %s", what, r.volumeName())
			case r.view != "" && !npfsBacked(v):
				add(errorProblem, i, rootsTab, "%s: view root needs an npfs packages volume", what)
			case r.readWrite && fatBacked(v):
				add(errorProblem, i, rootsTab, "%s: ordinary FAT32 roots must be read-only", what)
			}
		}

		for _, g := range s.grants {
			what := label + ": source " + g.name
			if !validSourceName(g.name) {
				add(errorProblem, i, grantsTab, "%s: source names use a-z, 0-9, '-' and '_'; management and inspection names are reserved", what)
			}
			if g.volume == "" || len(g.volume) > nameMax || !printable(g.volume) {
				add(errorProblem, i, grantsTab, "%s: volume must be 1 to %d printable characters", what, nameMax)
			}
			if g.kind != "directory" && g.kind != "mount" {
				add(errorProblem, i, grantsTab, "%s: kind must be directory or mount", what)
			}
			if g.path != "" {
				if g.kind != "directory" {
					add(errorProblem, i, grantsTab, "%s: a mount source cannot have a path", what)
				} else if len(g.path) > sourcePathMax || !printable(g.path) || !validSourcePath(g.path) {
					add(errorProblem, i, grantsTab, "%s: path must contain 1 to %d relative components without empty, . or ..", what, sourceComponents)
				}
			}
			v := c.volume(g.volume)
			if v == nil {
				add(errorProblem, i, grantsTab, "%s names undefined volume %s", what, g.volume)
				continue
			}
			if g.name == httpStagingSource && (g.kind != "directory" || !g.readWrite ||
				(v.kind != "ram" && !npfsBacked(v)) || !c.privateHTTPBacking(v)) {
				add(errorProblem, i, grantsTab, "%s requires a private read-write RAM or NPFS volume", what)
			}
			if g.kind == "mount" && !fatBacked(v) {
				add(errorProblem, i, grantsTab, "%s: a mount source requires a FAT32 volume", what)
			}
			if g.readWrite && fatBacked(v) && !(v.kind == "fat32" && v.readWrite) {
				add(errorProblem, i, grantsTab, "%s: a writable source requires read-write FAT32 admission", what)
			}
		}
	}

	if c.volume(systemVolume) != nil {
		keeps := false
		for _, s := range c.spaces {
			if r := s.root(systemVolume); r != nil && r.view == "" && r.readWrite {
				keeps = true
			}
		}
		if !keeps {
			add(warningProblem, -1, 0, "no space keeps system read-write; after the next boot only the rescue entry can edit the configuration")
		}
	}
	for _, u := range c.unknownKeys() {
		add(warningProblem, u.space, settingsTab, "%s", u.describe(c))
	}
	return problems
}

func countErrors(problems []problem) int {
	count := 0
	for _, p := range problems {
		if p.severity == errorProblem {
			count++
		}
	}
	return count
}
