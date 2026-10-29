// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

// The boot.lua model. Spaces are fully modelled; every other top-level value
// is kept as parsed and written back unchanged. Keys this editor does not know
// stay in place as extras and are reported as warnings (chronium, 2026-10-29).
// The schema mirrors userspace/libconfig/boot.c; change both together.

import (
	"fmt"
	"strings"
)

// flag is one boolean space setting, in written order.
type flag struct {
	key, group, label string
}

var flags = []flag{
	{"network", "Network", "Owns the network: policy, addresses and DHCP"},
	{"tcp_listen", "Network", "TCP LISTEN source for declaring bundles"},
	{"focus", "Launch", "Selected at boot"},
	{"launch", "Launch", "Ordinary commands can launch programs"},
	{"jobs", "Launch", "Private job contexts for declaring bundles"},
	{"jit", "Launch", "Generated-code source for declaring bundles"},
	{"multiplexer", "Terminal", "Starts the multiplexer"},
	{"clipboard_local", "Terminal", "Space-local clipboard"},
	{"clipboard_shared", "Terminal", "Clipboard shared between spaces"},
	{"screenshot", "System", "Whole-screen capture and Print Screen"},
	{"acpi_tables", "System", "ACPI tables as acpi://"},
	{"power", "System", "Power off and restart"},
	{"package_management", "Packages", "pkg manages the packages volume"},
	{"package_upgrade", "Packages", "pkg upgrade writes the boot ESP"},
	{"remote_multiplexer", "Remote", "Remote pane broker"},
	{"remote_power", "Remote", "Remote root shells may power off"},
}

var wifiRights = []string{"read", "scan", "connect"}

const (
	accessReadOnly  = "read-only"
	accessReadWrite = "read-write"
)

type root struct {
	name      string
	view      string // "", "pkg" or "sdk"
	readWrite bool
	optional  bool
	extra     []entry
}

type grant struct {
	name      string
	volume    string
	kind      string // "directory" or "mount"
	path      string
	readWrite bool
	optional  bool
	extra     []entry
}

type space struct {
	name, title, init, start string
	cpus                     []int64 // nil means every CPU
	flags                    map[string]bool
	wifi                     map[string]bool
	fat32Mount               string // "", accessReadOnly or accessReadWrite
	roots                    []*root
	grants                   []*grant
	extra                    []entry
}

type volume struct {
	name, kind string
	readWrite  bool // FAT32 admission mode
	partition  int64
	store      string // npfs and ram-npfs volume name
}

type config struct {
	top     *value // the parsed document; its spaces entry is regenerated
	volumes []volume
	spaces  []*space
}

var topKeys = map[string]bool{
	"hostname": true, "input": true, "volumes": true, "spaces": true,
	"ram_npfs_bytes": true, "wifi_policy": true,
}

// unknownKey is a key the editor carries through without understanding it.
type unknownKey struct {
	space int // -1 at the top level
	where string
	key   string
}

func keyText(key *value) string {
	if key.kind == stringKind {
		return key.text
	}
	var out strings.Builder
	writeValue(&out, key, 0)
	return "[" + out.String() + "]"
}

func valueText(v *value) string {
	var out strings.Builder
	writeValue(&out, v, 0)
	return strings.Join(strings.Fields(out.String()), " ")
}

type decodeError struct{ text string }

func (e *decodeError) Error() string { return e.text }

func failf(format string, arguments ...any) {
	panic(&decodeError{fmt.Sprintf(format, arguments...)})
}

// decodeConfig builds the model. Values of a known key with the wrong type
// cannot be edited and are a load error; boot would reject them too.
func decodeConfig(top *value) (result *config, err error) {
	defer func() {
		if failure, ok := recover().(*decodeError); ok {
			result, err = nil, failure
		} else if failure != nil {
			panic(failure)
		}
	}()
	c := &config{top: top}
	if volumes := top.field("volumes"); volumes != nil {
		if volumes.kind != tableKind {
			failf("volumes must be a table of named volumes")
		}
		for _, e := range volumes.entries {
			c.volumes = append(c.volumes, decodeVolume(e))
		}
	}
	if spaces := top.field("spaces"); spaces != nil {
		if spaces.kind != tableKind {
			failf("spaces must be a list")
		}
		for i, e := range spaces.entries {
			if e.key.kind != integerKind || e.key.integer != int64(i+1) {
				failf("spaces must be a list without other keys")
			}
			c.spaces = append(c.spaces, decodeSpace(e.value, i))
		}
	}
	return c, nil
}

func decodeVolume(e entry) volume {
	v := volume{name: keyText(e.key)}
	if e.value.kind != tableKind {
		return v
	}
	if kind := e.value.field("kind"); kind != nil && kind.kind == stringKind {
		v.kind = kind.text
	}
	if access := e.value.field("access"); access != nil && access.kind == stringKind {
		v.readWrite = access.text == accessReadWrite
	}
	if partition := e.value.field("partition"); partition != nil && partition.kind == integerKind {
		v.partition = partition.integer
	}
	if store := e.value.field("volume"); store != nil && store.kind == stringKind {
		v.store = store.text
	}
	return v
}

func takeString(v *value, what string) string {
	if v.kind != stringKind {
		failf("%s must be a string", what)
	}
	return v.text
}

func takeBool(v *value, what string) bool {
	if v.kind != booleanKind {
		failf("%s must be a boolean", what)
	}
	return v.boolean
}

func takeAccess(v *value, what string) bool {
	text := takeString(v, what)
	if text != accessReadOnly && text != accessReadWrite {
		failf("%s must be read-only or read-write", what)
	}
	return text == accessReadWrite
}

func isFlag(key string) bool {
	for _, f := range flags {
		if f.key == key {
			return true
		}
	}
	return false
}

func decodeSpace(v *value, index int) *space {
	if v.kind != tableKind {
		failf("spaces must be tables")
	}
	s := &space{flags: map[string]bool{}, wifi: map[string]bool{}}
	label := fmt.Sprintf("space %d", index+1)
	if name := v.field("name"); name != nil {
		s.name = takeString(name, label+": name")
		label = "space " + s.name
	}
	clipboard := map[string]bool{}
	for _, e := range v.entries {
		key := keyText(e.key)
		what := label + ": " + key
		switch {
		case e.key.kind != stringKind:
			s.extra = append(s.extra, e)
		case key == "name":
		case key == "title":
			s.title = takeString(e.value, what)
		case key == "init":
			s.init = takeString(e.value, what)
		case key == "start":
			s.start = takeString(e.value, what)
		case key == "cpus":
			s.cpus = decodeCPUs(e.value, what)
		case key == "fat32_mount":
			if takeAccess(e.value, what) {
				s.fat32Mount = accessReadWrite
			} else {
				s.fat32Mount = accessReadOnly
			}
		case key == "wifi":
			if e.value.kind != tableKind {
				failf("%s must be a table", what)
			}
			for _, right := range e.value.entries {
				name := keyText(right.key)
				known := false
				for _, r := range wifiRights {
					known = known || r == name
				}
				if !known || right.key.kind != stringKind {
					failf("%s: unknown right %s", what, name)
				}
				s.wifi[name] = takeBool(right.value, what+"."+name)
			}
		case key == "roots":
			if e.value.kind != tableKind {
				failf("%s must be a table of volume names", what)
			}
			for _, r := range e.value.entries {
				s.roots = append(s.roots, decodeRoot(r, what))
			}
		case key == "grants":
			if e.value.kind != tableKind {
				failf("%s must be a table of named sources", what)
			}
			for _, g := range e.value.entries {
				s.grants = append(s.grants, decodeGrant(g, what))
			}
		case isFlag(key):
			s.flags[key] = takeBool(e.value, what)
			if key == "clipboard_local" || key == "clipboard_shared" {
				clipboard[key] = true
			}
		default:
			s.extra = append(s.extra, e)
		}
	}
	// An absent clipboard setting follows multiplexer; writing both keeps that.
	for _, key := range []string{"clipboard_local", "clipboard_shared"} {
		if !clipboard[key] {
			s.flags[key] = s.flags["multiplexer"]
		}
	}
	return s
}

func decodeCPUs(v *value, what string) []int64 {
	if v.kind != tableKind {
		failf("%s must be a list", what)
	}
	cpus := []int64{}
	for i, e := range v.entries {
		if e.key.kind != integerKind || e.key.integer != int64(i+1) || e.value.kind != integerKind {
			failf("%s must be a list of CPU indices", what)
		}
		cpus = append(cpus, e.value.integer)
	}
	return cpus
}

func decodeRoot(e entry, what string) *root {
	r := &root{name: keyText(e.key)}
	what += " " + r.name
	if e.value.kind != tableKind {
		r.readWrite = takeAccess(e.value, what)
		return r
	}
	access := ""
	for _, field := range e.value.entries {
		key := keyText(field.key)
		switch {
		case field.key.kind == stringKind && key == "access":
			access = takeString(field.value, what+": access")
		case field.key.kind == stringKind && key == "optional":
			r.optional = takeBool(field.value, what+": optional")
		case field.key.kind == stringKind && key == "view":
			r.view = takeString(field.value, what+": view")
		default:
			r.extra = append(r.extra, field)
		}
	}
	if r.view == "" && access != accessReadOnly && access != accessReadWrite {
		failf("%s: access must be read-only or read-write", what)
	}
	if r.view != "" && access != "" && access != accessReadOnly {
		failf("%s: view roots are read-only", what)
	}
	r.readWrite = access == accessReadWrite
	return r
}

func decodeGrant(e entry, what string) *grant {
	g := &grant{name: keyText(e.key)}
	what += " " + g.name
	if e.value.kind != tableKind {
		failf("%s must be a table", what)
	}
	for _, field := range e.value.entries {
		key := keyText(field.key)
		switch {
		case field.key.kind == stringKind && key == "volume":
			g.volume = takeString(field.value, what+": volume")
		case field.key.kind == stringKind && key == "kind":
			g.kind = takeString(field.value, what+": kind")
		case field.key.kind == stringKind && key == "path":
			g.path = takeString(field.value, what+": path")
		case field.key.kind == stringKind && key == "access":
			g.readWrite = takeAccess(field.value, what+": access")
		case field.key.kind == stringKind && key == "optional":
			g.optional = takeBool(field.value, what+": optional")
		default:
			g.extra = append(g.extra, field)
		}
	}
	return g
}

// unknownKeys lists every carried-through key, in file order.
func (c *config) unknownKeys() []unknownKey {
	var keys []unknownKey
	for _, e := range c.top.entries {
		if e.key.kind != stringKind || !topKeys[e.key.text] {
			keys = append(keys, unknownKey{-1, "", keyText(e.key)})
		}
	}
	for i, s := range c.spaces {
		for _, e := range s.extra {
			keys = append(keys, unknownKey{i, "", keyText(e.key)})
		}
		for _, r := range s.roots {
			for _, e := range r.extra {
				keys = append(keys, unknownKey{i, "root " + r.name, keyText(e.key)})
			}
		}
		for _, g := range s.grants {
			for _, e := range g.extra {
				keys = append(keys, unknownKey{i, "grant " + g.name, keyText(e.key)})
			}
		}
	}
	return keys
}

func (u unknownKey) describe(c *config) string {
	place := "top level"
	if u.space >= 0 {
		place = "space " + c.spaces[u.space].name
		if u.where != "" {
			place += ", " + u.where
		}
	}
	return fmt.Sprintf("%s: unknown key %s is kept unchanged", place, u.key)
}

func encodeSpace(s *space) *value {
	v := tableValue()
	v.set("name", stringValue(s.name))
	if s.title != "" {
		v.set("title", stringValue(s.title))
	}
	v.set("init", stringValue(s.init))
	if s.cpus != nil {
		cpus := tableValue()
		for _, cpu := range s.cpus {
			cpus.appendItem(integerValue(cpu))
		}
		v.set("cpus", cpus)
	}
	if s.start != "" {
		v.set("start", stringValue(s.start))
	}
	for _, f := range flags {
		clipboard := f.key == "clipboard_local" || f.key == "clipboard_shared"
		if s.flags[f.key] || clipboard {
			v.set(f.key, boolValue(s.flags[f.key]))
		}
	}
	wifi := tableValue()
	for _, right := range wifiRights {
		if s.wifi[right] {
			wifi.set(right, boolValue(true))
		}
	}
	if len(wifi.entries) > 0 {
		v.set("wifi", wifi)
	}
	if s.fat32Mount != "" {
		v.set("fat32_mount", stringValue(s.fat32Mount))
	}
	if len(s.grants) > 0 {
		grants := tableValue()
		for _, g := range s.grants {
			grants.set(g.name, encodeGrant(g))
		}
		v.set("grants", grants)
	}
	if len(s.roots) > 0 {
		roots := tableValue()
		for _, r := range s.roots {
			roots.set(r.name, encodeRoot(r))
		}
		v.set("roots", roots)
	}
	v.entries = append(v.entries, s.extra...)
	return v
}

func access(readWrite bool) string {
	if readWrite {
		return accessReadWrite
	}
	return accessReadOnly
}

func encodeRoot(r *root) *value {
	if r.view == "" && !r.optional && len(r.extra) == 0 {
		return stringValue(access(r.readWrite))
	}
	v := tableValue()
	if r.view != "" {
		v.set("view", stringValue(r.view))
	} else {
		v.set("access", stringValue(access(r.readWrite)))
	}
	if r.optional {
		v.set("optional", boolValue(true))
	}
	v.entries = append(v.entries, r.extra...)
	return v
}

func encodeGrant(g *grant) *value {
	v := tableValue()
	v.set("volume", stringValue(g.volume))
	v.set("kind", stringValue(g.kind))
	if g.path != "" {
		v.set("path", stringValue(g.path))
	}
	v.set("access", stringValue(access(g.readWrite)))
	if g.optional {
		v.set("optional", boolValue(true))
	}
	v.entries = append(v.entries, g.extra...)
	return v
}

// encode regenerates the spaces list in its original position, or at the end.
func (c *config) encode() string {
	spaces := tableValue()
	for _, s := range c.spaces {
		spaces.appendItem(encodeSpace(s))
	}
	top := &value{kind: tableKind}
	placed := false
	for _, e := range c.top.entries {
		if e.key.kind == stringKind && e.key.text == "spaces" {
			e.value, placed = spaces, true
		}
		top.entries = append(top.entries, e)
	}
	if !placed && len(c.spaces) > 0 {
		top.set("spaces", spaces)
	}
	return writeLiteral(top)
}

func (c *config) volume(name string) *volume {
	for i := range c.volumes {
		if c.volumes[i].name == name {
			return &c.volumes[i]
		}
	}
	return nil
}

func (s *space) root(name string) *root {
	for _, r := range s.roots {
		if r.name == name {
			return r
		}
	}
	return nil
}

func (s *space) copy() *space {
	c := *s
	c.cpus = append([]int64(nil), s.cpus...)
	if s.cpus == nil {
		c.cpus = nil
	}
	c.flags, c.wifi = map[string]bool{}, map[string]bool{}
	for k, v := range s.flags {
		c.flags[k] = v
	}
	for k, v := range s.wifi {
		c.wifi[k] = v
	}
	c.roots, c.grants = nil, nil
	for _, r := range s.roots {
		copy := *r
		c.roots = append(c.roots, &copy)
	}
	for _, g := range s.grants {
		copy := *g
		c.grants = append(c.grants, &copy)
	}
	c.extra = append([]entry(nil), s.extra...)
	return &c
}
