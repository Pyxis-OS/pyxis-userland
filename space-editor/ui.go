// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package main

import (
	"fmt"
	"sort"
	"strconv"
	"strings"

	"github.com/gdamore/tcell/v2"
	"github.com/rivo/tview"
)

// Colours are palette indices so the terminal's scheme (Aardvark) applies.
var (
	colorAccent  = tcell.PaletteColor(14)
	colorHeading = tcell.PaletteColor(11)
	colorDim     = tcell.PaletteColor(8)
	colorError   = tcell.PaletteColor(9)
	colorWarning = tcell.PaletteColor(11)
	colorAdded   = tcell.PaletteColor(10)
	colorSelect  = tcell.PaletteColor(4)
)

var tabNames = []string{"General", "Settings", "Roots", "Grants"}

type editor struct {
	app      *tview.Application
	pages    *tview.Pages
	path     string
	loaded   []byte
	cfg      *config
	dirty    bool
	readOnly error
	problems []problem

	header, footer, status *tview.TextView
	list                   *tview.Table

	detailHeader, tabBar *tview.TextView
	detail               *tview.Table
	detailStatus         *tview.TextView
	detailFooter         *tview.TextView
	space, tab           int
	// rows maps detail table rows to what they edit.
	rows []func()
}

func setTheme() {
	tview.Styles = tview.Theme{
		PrimitiveBackgroundColor:    tcell.ColorDefault,
		ContrastBackgroundColor:     colorSelect,
		MoreContrastBackgroundColor: tcell.PaletteColor(6),
		BorderColor:                 colorDim,
		TitleColor:                  colorHeading,
		GraphicsColor:               colorDim,
		PrimaryTextColor:            tcell.ColorDefault,
		SecondaryTextColor:          colorHeading,
		TertiaryTextColor:           colorAccent,
		InverseTextColor:            tcell.PaletteColor(15),
		ContrastSecondaryTextColor:  colorHeading,
	}
}

func newEditor(path string, loaded []byte, cfg *config, readOnly error) *editor {
	setTheme()
	e := &editor{app: tview.NewApplication(), pages: tview.NewPages(), path: path,
		loaded: loaded, cfg: cfg, readOnly: readOnly}
	e.buildList()
	e.buildDetail()
	e.app.SetRoot(e.pages, true)
	e.app.SetInputCapture(e.globalKeys)
	e.refresh()
	return e
}

func (e *editor) run() error { return e.app.Run() }

// ---- shared pieces --------------------------------------------------------

func textLine() *tview.TextView {
	return tview.NewTextView().SetDynamicColors(true).SetWrap(false)
}

func tableBox(title string) *tview.Table {
	t := tview.NewTable().SetSelectable(true, false).SetFixed(1, 0)
	t.SetBorder(true).SetTitle(" " + title + " ").SetTitleAlign(tview.AlignLeft)
	t.SetSelectedStyle(tcell.StyleDefault.Background(colorSelect).Foreground(tcell.PaletteColor(15)))
	return t
}

func headingCell(text string) *tview.TableCell {
	return tview.NewTableCell(text).SetTextColor(colorHeading).SetSelectable(false).
		SetAttributes(tcell.AttrBold)
}

func cell(text string) *tview.TableCell { return tview.NewTableCell(tview.Escape(text)) }

func dimCell(text string) *tview.TableCell { return cell(text).SetTextColor(colorDim) }

func (e *editor) headerText(title string) string {
	text := fmt.Sprintf(" [::b]%s[::-]  %s", title, tview.Escape(e.path))
	if e.dirty {
		text += "  " + tag(colorWarning) + "modified[-]"
	}
	if e.readOnly != nil {
		text += "  " + tag(colorError) + "read-only: saving disabled[-]"
	}
	return text
}

// Dynamic colour tags name the same palette entries.
var colorTags = map[tcell.Color]string{
	colorAccent: "aqua", colorHeading: "yellow", colorDim: "gray", colorError: "red", colorAdded: "lime",
}

func tag(c tcell.Color) string { return "[" + colorTags[c] + "]" }

// centered places a modal of the given size over the current page.
func centered(p tview.Primitive, width, height int) tview.Primitive {
	return tview.NewFlex().
		AddItem(nil, 0, 1, false).
		AddItem(tview.NewFlex().SetDirection(tview.FlexRow).
			AddItem(nil, 0, 1, false).
			AddItem(p, height, 0, true).
			AddItem(nil, 0, 1, false), width, 0, true).
		AddItem(nil, 0, 1, false)
}

func (e *editor) showModal(name string, p tview.Primitive, width, height int) {
	e.pages.AddPage(name, centered(p, width, height), true, true)
}

func (e *editor) closeModal(name string) {
	e.pages.RemovePage(name)
}

func (e *editor) message(title, text string) {
	m := tview.NewModal().SetText(text).AddButtons([]string{"OK"}).
		SetDoneFunc(func(int, string) { e.pages.RemovePage("message") })
	m.SetTitle(" " + title + " ")
	e.pages.AddPage("message", m, true, true)
}

func modalForm(title string) *tview.Form {
	f := tview.NewForm()
	f.SetBorder(true).SetTitle(" " + title + " ").SetTitleAlign(tview.AlignLeft)
	f.SetItemPadding(0)
	f.SetFieldBackgroundColor(colorSelect).SetFieldTextColor(tcell.PaletteColor(15)).
		SetButtonBackgroundColor(colorSelect).SetButtonTextColor(tcell.PaletteColor(15)).
		SetLabelColor(colorHeading)
	return f
}

func (e *editor) topModal() bool {
	name, _ := e.pages.GetFrontPage()
	return name != "list" && name != "detail"
}

func (e *editor) changed() {
	e.dirty = true
	e.refresh()
}

// ---- global keys -----------------------------------------------------------

func (e *editor) globalKeys(event *tcell.EventKey) *tcell.EventKey {
	switch event.Key() {
	case tcell.KeyCtrlC, tcell.KeyCtrlQ:
		if !e.topModal() {
			e.quit()
		}
		return nil
	case tcell.KeyCtrlS:
		if !e.topModal() {
			e.review()
		}
		return nil
	case tcell.KeyCtrlR:
		if !e.topModal() {
			e.discard()
		}
		return nil
	case tcell.KeyCtrlP:
		if !e.topModal() {
			e.loadPrevious()
		}
		return nil
	}
	if event.Key() == tcell.KeyRune && event.Rune() == '?' && !e.topModal() {
		e.help()
		return nil
	}
	return event
}

// ---- space list ------------------------------------------------------------

func (e *editor) buildList() {
	e.header, e.footer, e.status = textLine(), textLine(), textLine()
	e.list = tableBox("Spaces (tab order)")
	e.list.SetSelectedFunc(func(row, _ int) { e.openSpace(row - 1) })
	e.list.SetInputCapture(func(event *tcell.EventKey) *tcell.EventKey {
		index := e.selectedSpace()
		switch event.Key() {
		case tcell.KeyDelete:
			if index >= 0 {
				e.removeSpace(index)
			}
			return nil
		case tcell.KeyRune:
			switch event.Rune() {
			case 'a':
				e.addSpace(-1)
			case 'c':
				if index >= 0 {
					e.addSpace(index)
				}
			case 'r':
				if index >= 0 {
					e.renameSpace(index)
				}
			case '[':
				e.moveSpace(index, -1)
			case ']':
				e.moveSpace(index, 1)
			default:
				return event
			}
			return nil
		}
		return event
	})
	e.footer.SetText(" " + strings.Join([]string{
		"a add", "c copy", "r rename", "Del remove", "[ ] move",
		"^S save", "? help", "^Q quit"}, "  "))
	e.footer.SetTextColor(colorDim)
	layout := tview.NewFlex().SetDirection(tview.FlexRow).
		AddItem(e.header, 1, 0, false).
		AddItem(e.list, 0, 1, true).
		AddItem(e.status, 1, 0, false).
		AddItem(e.footer, 1, 0, false)
	e.pages.AddPage("list", layout, true, true)
}

func (e *editor) selectedSpace() int {
	row, _ := e.list.GetSelection()
	if row < 1 || row > len(e.cfg.spaces) {
		return -1
	}
	return row - 1
}

func markers(s *space) string {
	var parts []string
	for _, m := range []struct{ key, text string }{
		{"network", "network"}, {"focus", "focus"}, {"launch", "launch"}, {"multiplexer", "mux"},
		{"power", "power"}, {"package_management", "pkg"}, {"screenshot", "screenshot"},
	} {
		if s.flags[m.key] {
			parts = append(parts, m.text)
		}
	}
	return strings.Join(parts, " ")
}

func (e *editor) spaceProblems(index int) (errors, warnings int) {
	for _, p := range e.problems {
		if p.space != index {
			continue
		}
		if p.severity == errorProblem {
			errors++
		} else {
			warnings++
		}
	}
	return
}

func problemMark(errors, warnings int) *tview.TableCell {
	switch {
	case errors > 0:
		return cell("!!").SetTextColor(colorError)
	case warnings > 0:
		return cell("!").SetTextColor(colorWarning)
	}
	return cell("")
}

func (e *editor) refreshList() {
	selected := e.selectedSpace()
	e.header.SetText(e.headerText("Spaces"))
	t := e.list
	t.Clear()
	for column, text := range []string{"#", "", "Name", "Title", "Roots", "Grants", "Settings"} {
		t.SetCell(0, column, headingCell(text))
	}
	for i, s := range e.cfg.spaces {
		errors, warnings := e.spaceProblems(i)
		title := s.title
		if title == "" {
			title = s.name
		}
		t.SetCell(i+1, 0, dimCell(strconv.Itoa(i+1)))
		t.SetCell(i+1, 1, problemMark(errors, warnings))
		t.SetCell(i+1, 2, cell(s.name).SetTextColor(colorAccent))
		t.SetCell(i+1, 3, cell(title))
		t.SetCell(i+1, 4, cell(strconv.Itoa(len(s.roots))).SetAlign(tview.AlignRight))
		t.SetCell(i+1, 5, cell(strconv.Itoa(len(s.grants))).SetAlign(tview.AlignRight))
		t.SetCell(i+1, 6, cell(markers(s)).SetExpansion(1))
	}
	if len(e.cfg.spaces) == 0 {
		t.SetCell(1, 2, dimCell("no spaces: press a to add one").SetSelectable(false))
	}
	if selected < 0 {
		selected = 0
	}
	if selected >= len(e.cfg.spaces) {
		selected = len(e.cfg.spaces) - 1
	}
	t.Select(selected+1, 0)
	e.status.SetText(e.problemSummary(-1))
}

func (e *editor) problemSummary(index int) string {
	errors, warnings := 0, 0
	first := ""
	for _, p := range e.problems {
		if index >= 0 && p.space != index {
			continue
		}
		if p.severity == errorProblem {
			errors++
		} else {
			warnings++
		}
		if first == "" || (p.severity == errorProblem && !strings.HasPrefix(first, tag(colorError))) {
			color := colorWarning
			if p.severity == errorProblem {
				color = colorError
			}
			first = tag(color) + tview.Escape(p.text) + "[-]"
		}
	}
	if errors+warnings == 0 {
		return " " + tag(colorAdded) + "No problems found.[-]"
	}
	return fmt.Sprintf(" %d error(s), %d warning(s): %s", errors, warnings, first)
}

// ---- space detail ----------------------------------------------------------

func (e *editor) buildDetail() {
	e.detailHeader, e.tabBar = textLine(), textLine()
	e.detail = tableBox("")
	e.detail.SetSelectedFunc(func(row, _ int) {
		if row < len(e.rows) && e.rows[row] != nil {
			e.rows[row]()
		}
	})
	e.detail.SetInputCapture(e.detailKeys)
	status, footer := textLine(), textLine()
	footer.SetTextColor(colorDim)
	layout := tview.NewFlex().SetDirection(tview.FlexRow).
		AddItem(e.detailHeader, 1, 0, false).
		AddItem(e.tabBar, 1, 0, false).
		AddItem(e.detail, 0, 1, true).
		AddItem(status, 1, 0, false).
		AddItem(footer, 1, 0, false)
	e.pages.AddPage("detail", layout, true, false)
	e.detailStatus, e.detailFooter = status, footer
}

func (e *editor) openSpace(index int) {
	if index < 0 || index >= len(e.cfg.spaces) {
		return
	}
	e.space = index
	e.showTab(generalTab)
	e.pages.SwitchToPage("detail")
}

func (e *editor) showTab(tab int) {
	e.tab = tab
	e.refreshDetail()
	e.detail.Select(1, 0)
	e.detail.ScrollToBeginning()
	e.selectFirstEditable()
}

func (e *editor) selectFirstEditable() {
	for row := 1; row < e.detail.GetRowCount(); row++ {
		if c := e.detail.GetCell(row, 0); c != nil && c.NotSelectable {
			continue
		}
		e.detail.Select(row, 0)
		return
	}
}

func (e *editor) detailKeys(event *tcell.EventKey) *tcell.EventKey {
	s := e.cfg.spaces[e.space]
	row, _ := e.detail.GetSelection()
	switch event.Key() {
	case tcell.KeyEscape:
		e.pages.SwitchToPage("list")
		e.refresh()
		return nil
	case tcell.KeyDelete:
		e.removeEntry(s, row)
		return nil
	case tcell.KeyRune:
		switch r := event.Rune(); {
		case r >= '1' && r <= '4':
			e.showTab(int(r - '1'))
			return nil
		case r == ' ' && e.tab == settingsTab:
			if row < len(e.rows) && e.rows[row] != nil {
				e.rows[row]()
			}
			return nil
		case r == 'a' && e.tab == rootsTab:
			e.editRoot(s, nil)
			return nil
		case r == 'a' && e.tab == grantsTab:
			e.editGrant(s, nil)
			return nil
		case r == 'e' && (e.tab == rootsTab || e.tab == grantsTab):
			if row < len(e.rows) && e.rows[row] != nil {
				e.rows[row]()
			}
			return nil
		}
	}
	return event
}

func (e *editor) refreshDetail() {
	if e.space >= len(e.cfg.spaces) {
		return
	}
	s := e.cfg.spaces[e.space]
	title := s.title
	if title == "" {
		title = s.name
	}
	e.detailHeader.SetText(e.headerText("Space "+s.name) + "  " + tag(colorDim) + tview.Escape("("+title+")") + "[-]")
	var bar []string
	for i, name := range tabNames {
		if i == e.tab {
			bar = append(bar, fmt.Sprintf("[::r] %d %s [::-]", i+1, name))
		} else {
			bar = append(bar, fmt.Sprintf(" %d %s ", i+1, name))
		}
	}
	e.tabBar.SetText(" " + strings.Join(bar, " "))
	row, _ := e.detail.GetSelection()
	t := e.detail
	t.Clear()
	e.rows = nil
	switch e.tab {
	case generalTab:
		e.fillGeneral(s)
	case settingsTab:
		e.fillSettings(s)
	case rootsTab:
		e.fillRoots(s)
	case grantsTab:
		e.fillGrants(s)
	}
	title = tabNames[e.tab]
	if e.tab == rootsTab {
		title += " (every space also has boot://, tmp:// and bin://)"
	}
	t.SetTitle(" " + title + " ")
	if row >= t.GetRowCount() {
		row = t.GetRowCount() - 1
	}
	if row >= 1 {
		t.Select(row, 0)
	}
	e.detailStatus.SetText(e.problemSummary(e.space))
	hints := map[int][]string{
		generalTab:  {"Enter edit"},
		settingsTab: {"Space/Enter toggle"},
		rootsTab:    {"a add", "e/Enter edit", "Del remove"},
		grantsTab:   {"a add", "e/Enter edit", "Del remove"},
	}[e.tab]
	hints = append(hints, "1-4 tabs", "Esc back", "^S save", "? help")
	e.detailFooter.SetText(" " + strings.Join(hints, "  "))
}

func (e *editor) addRow(action func(), cells ...*tview.TableCell) {
	row := e.detail.GetRowCount()
	for column, c := range cells {
		e.detail.SetCell(row, column, c)
	}
	for len(e.rows) < row {
		e.rows = append(e.rows, nil)
	}
	e.rows = append(e.rows, action)
}

func (e *editor) fillGeneral(s *space) {
	e.addRow(nil, headingCell("Setting"), headingCell("Value"), headingCell(""))
	title, titleNote := s.title, ""
	if title == "" {
		title, titleNote = s.name, "defaults to the name"
	}
	cpus, cpusNote := formatCPUs(s.cpus), ""
	if s.cpus == nil {
		cpus, cpusNote = "all", "every boot CPU"
	}
	start, startNote := s.start, ""
	if start == "" {
		start, startNote = "home", "default; tmp:// without a home root"
	}
	rows := []struct {
		label, value, note string
		edit               func()
	}{
		{"Name", s.name, "identifies the space in logs", func() { e.renameSpace(e.space) }},
		{"Title", title, titleNote, func() {
			e.editText("Title", "Tab label, 1-63 printable characters; empty uses the name", s.title, func(text string) { s.title = text })
		}},
		{"Init", s.init, "boot:// entry or system:// file", func() {
			e.editText("Init", "The space's init script or program", s.init, func(text string) { s.init = text })
		}},
		{"Start", start, startNote, func() {
			e.editText("Start", "Root the space starts in; empty means home", s.start, func(text string) { s.start = text })
		}},
		{"CPUs", cpus, cpusNote, func() {
			e.editText("CPUs", "Boot CPU indices, such as 1,2,3; empty means every CPU", formatCPUs(s.cpus), func(text string) {
				s.cpus = parseCPUs(text)
			})
		}},
	}
	for _, r := range rows {
		e.addRow(r.edit, cell(r.label).SetTextColor(colorHeading), cell(r.value).SetExpansion(1), dimCell(r.note))
	}
}

func formatCPUs(cpus []int64) string {
	var parts []string
	for _, cpu := range cpus {
		parts = append(parts, strconv.FormatInt(cpu, 10))
	}
	return strings.Join(parts, ",")
}

// parseCPUs keeps unparseable entries as -1 so validation reports them.
func parseCPUs(text string) []int64 {
	text = strings.TrimSpace(text)
	if text == "" {
		return nil
	}
	cpus := []int64{}
	for _, part := range strings.Split(text, ",") {
		cpu, err := strconv.ParseInt(strings.TrimSpace(part), 10, 64)
		if err != nil {
			cpu = -1
		}
		cpus = append(cpus, cpu)
	}
	return cpus
}

func checkbox(on bool) *tview.TableCell {
	if on {
		return cell("[x]").SetTextColor(colorAccent)
	}
	return cell("[ ]")
}

func (e *editor) fillSettings(s *space) {
	group := ""
	for _, f := range flags {
		if f.group != group {
			group = f.group
			e.addRow(nil, headingCell(""), headingCell(group), headingCell(""))
			if group == "Network" {
				e.wifiRows(s)
			}
		}
		key := f.key
		e.addRow(func() { s.flags[key] = !s.flags[key]; e.changed() },
			checkbox(s.flags[key]), cell(key), dimCell(f.label).SetExpansion(1))
	}
	e.addRow(nil, headingCell(""), headingCell("Disks"), headingCell(""))
	mark := map[string]string{"": "[-]", accessReadOnly: "[r]", accessReadWrite: "[w]"}[s.fat32Mount]
	mount := s.fat32Mount
	if mount == "" {
		mount = "absent"
	}
	e.addRow(func() {
		next := map[string]string{"": accessReadOnly, accessReadOnly: accessReadWrite, accessReadWrite: ""}
		s.fat32Mount = next[s.fat32Mount]
		e.changed()
	}, cell(mark).SetTextColor(colorAccent), cell("fat32_mount"), dimCell("FAT32 mount-only factory: "+mount+" (Enter cycles)").SetExpansion(1))
	if len(s.extra) > 0 {
		e.addRow(nil, headingCell(""), headingCell("Unknown").SetTextColor(colorWarning), dimCell("kept unchanged").SetSelectable(false))
		for _, x := range s.extra {
			e.addRow(nil, cell("!").SetTextColor(colorWarning), cell(keyText(x.key)).SetTextColor(colorWarning),
				dimCell(valueText(x.value)).SetExpansion(1))
		}
	}
}

func (e *editor) wifiRows(s *space) {
	for _, right := range wifiRights {
		right := right
		e.addRow(func() { s.wifi[right] = !s.wifi[right]; e.changed() },
			checkbox(s.wifi[right]), cell("wifi."+right), dimCell("Wi-Fi controller "+strings.ToUpper(right)).SetExpansion(1))
	}
}

func yesNo(b bool) string {
	if b {
		return "yes"
	}
	return "no"
}

func (e *editor) fillRoots(s *space) {
	e.addRow(nil, headingCell("Root"), headingCell("Target"), headingCell("Access"), headingCell("Optional"), headingCell(""))
	for _, r := range s.roots {
		r := r
		target := "volume " + r.name
		if r.view != "" {
			target = r.view + " view of packages"
		}
		if e.cfg.volume(r.volumeName()) == nil {
			target += " (undefined)"
		}
		note := ""
		if len(r.extra) > 0 {
			note = fmt.Sprintf("! %d unknown key(s) kept", len(r.extra))
		}
		e.addRow(func() { e.editRoot(s, r) }, cell(r.name+"://").SetTextColor(colorAccent), cell(target),
			cell(access(r.readWrite)), cell(yesNo(r.optional)), cell(note).SetTextColor(colorWarning).SetExpansion(1))
	}
	if len(s.roots) == 0 {
		e.addRow(nil, dimCell("no roots: press a to add one").SetSelectable(false))
	}
}

func (e *editor) fillGrants(s *space) {
	e.addRow(nil, headingCell("Source"), headingCell("Volume"), headingCell("Kind"), headingCell("Access"),
		headingCell("Optional"), headingCell("Path"))
	for _, g := range s.grants {
		g := g
		path := g.path
		if len(g.extra) > 0 {
			path += fmt.Sprintf("  ! %d unknown key(s) kept", len(g.extra))
		}
		e.addRow(func() { e.editGrant(s, g) }, cell(g.name).SetTextColor(colorAccent), cell(g.volume), cell(g.kind),
			cell(access(g.readWrite)), cell(yesNo(g.optional)), cell(path).SetExpansion(1))
	}
	if len(s.grants) == 0 {
		e.addRow(nil, dimCell("no selected sources: press a to add one").SetSelectable(false))
	}
}

func (e *editor) removeEntry(s *space, row int) {
	index := row - 1
	switch {
	case e.tab == rootsTab && index >= 0 && index < len(s.roots):
		s.roots = append(s.roots[:index], s.roots[index+1:]...)
	case e.tab == grantsTab && index >= 0 && index < len(s.grants):
		s.grants = append(s.grants[:index], s.grants[index+1:]...)
	default:
		return
	}
	e.changed()
}

// ---- modals ----------------------------------------------------------------

// editText edits one General field; Enter accepts.
func (e *editor) editText(label, hint, current string, apply func(string)) {
	f := modalForm(label)
	f.AddTextView("", hint, 0, 1, false, false)
	f.AddInputField(label, current, 50, nil, nil)
	done := func() {
		apply(f.GetFormItem(1).(*tview.InputField).GetText())
		e.closeModal("edit")
		e.changed()
	}
	f.GetFormItem(1).(*tview.InputField).SetDoneFunc(func(key tcell.Key) {
		if key == tcell.KeyEnter {
			done()
		} else if key == tcell.KeyEscape {
			e.closeModal("edit")
		}
	})
	f.AddButton("OK", done).AddButton("Cancel", func() { e.closeModal("edit") })
	f.SetCancelFunc(func() { e.closeModal("edit") })
	f.SetFocus(1)
	e.showModal("edit", f, 72, 8)
}

func (e *editor) volumeNames() []string {
	var names []string
	for _, v := range e.cfg.volumes {
		names = append(names, v.name)
	}
	sort.Strings(names)
	return names
}

func indexOf(options []string, value string) int {
	for i, option := range options {
		if option == value {
			return i
		}
	}
	return 0
}

// editRoot adds a root (r == nil) or edits one. A root is named after its
// volume, or is the pkg or sdk view.
func (e *editor) editRoot(s *space, r *root) {
	title := "Add root"
	edited := &root{readWrite: false}
	if r != nil {
		title, *edited = "Edit root "+r.name, *r
	}
	targets := append(e.volumeNames(), "pkg view", "sdk view")
	current := edited.name
	if edited.view != "" {
		current = edited.view + " view"
	}
	f := modalForm(title)
	f.AddDropDown("Target", targets, indexOf(targets, current), nil)
	f.AddDropDown("Access", []string{accessReadOnly, accessReadWrite}, map[bool]int{false: 0, true: 1}[edited.readWrite], nil)
	f.AddCheckbox("Optional", edited.optional, nil)
	f.AddTextView("", "A missing optional root does not stop the space. View roots are read-only.", 0, 2, false, false)
	f.AddButton("Save", func() {
		_, target := f.GetFormItem(0).(*tview.DropDown).GetCurrentOption()
		_, mode := f.GetFormItem(1).(*tview.DropDown).GetCurrentOption()
		edited.view, edited.name = "", target
		if strings.HasSuffix(target, " view") {
			edited.view = strings.TrimSuffix(target, " view")
			edited.name = edited.view
		}
		edited.readWrite = mode == accessReadWrite && edited.view == ""
		edited.optional = f.GetFormItem(2).(*tview.Checkbox).IsChecked()
		for _, other := range s.roots {
			if other != r && other.name == edited.name {
				e.message("Root exists", "This space already has a root named "+edited.name+".")
				return
			}
		}
		if r == nil {
			s.roots = append(s.roots, edited)
		} else {
			*r = *edited
		}
		e.closeModal("root")
		e.changed()
	})
	f.AddButton("Cancel", func() { e.closeModal("root") })
	f.SetCancelFunc(func() { e.closeModal("root") })
	e.showModal("root", f, 64, 11)
}

func (e *editor) editGrant(s *space, g *grant) {
	title := "Add selected source"
	edited := &grant{kind: "directory"}
	if g != nil {
		title, *edited = "Edit source "+g.name, *g
	}
	volumes := e.volumeNames()
	kinds := []string{"directory", "mount"}
	f := modalForm(title)
	f.AddInputField("Name", edited.name, 32, nil, nil)
	f.AddDropDown("Volume", volumes, indexOf(volumes, edited.volume), nil)
	f.AddDropDown("Kind", kinds, indexOf(kinds, edited.kind), nil)
	f.AddDropDown("Access", []string{accessReadOnly, accessReadWrite}, map[bool]int{false: 0, true: 1}[edited.readWrite], nil)
	f.AddCheckbox("Optional", edited.optional, nil)
	f.AddInputField("Path", edited.path, 40, nil, nil)
	f.AddTextView("", "Path selects a relative subtree of a directory source; empty is the volume root.", 0, 2, false, false)
	f.AddButton("Save", func() {
		edited.name = strings.TrimSpace(f.GetFormItem(0).(*tview.InputField).GetText())
		_, edited.volume = f.GetFormItem(1).(*tview.DropDown).GetCurrentOption()
		_, edited.kind = f.GetFormItem(2).(*tview.DropDown).GetCurrentOption()
		_, mode := f.GetFormItem(3).(*tview.DropDown).GetCurrentOption()
		edited.readWrite = mode == accessReadWrite
		edited.optional = f.GetFormItem(4).(*tview.Checkbox).IsChecked()
		edited.path = strings.TrimSpace(f.GetFormItem(5).(*tview.InputField).GetText())
		if edited.name == "" {
			e.message("Name needed", "A selected source needs a name.")
			return
		}
		for _, other := range s.grants {
			if other != g && other.name == edited.name {
				e.message("Source exists", "This space already has a source named "+edited.name+".")
				return
			}
		}
		if g == nil {
			s.grants = append(s.grants, edited)
		} else {
			*g = *edited
		}
		e.closeModal("grant")
		e.changed()
	})
	f.AddButton("Cancel", func() { e.closeModal("grant") })
	f.SetCancelFunc(func() { e.closeModal("grant") })
	e.showModal("grant", f, 66, 14)
}

// addSpace adds a blank space, or a copy of space FROM.
func (e *editor) addSpace(from int) {
	title, name, spaceTitle, init := "Add space", "", "", "system://config/init-installed"
	if from >= 0 {
		source := e.cfg.spaces[from]
		title, name, spaceTitle, init = "Copy space "+source.name, source.name+"-copy", source.title, source.init
	}
	f := modalForm(title)
	f.AddInputField("Name", name, 31, nil, nil)
	f.AddInputField("Title", spaceTitle, 40, nil, nil)
	f.AddInputField("Init", init, 44, nil, nil)
	f.AddTextView("", "Names use a-z, 0-9 and '-'. The new space goes after the selected one.", 0, 2, false, false)
	f.AddButton("Add", func() {
		var s *space
		if from >= 0 {
			s = e.cfg.spaces[from].copy()
		} else {
			s = &space{flags: map[string]bool{}, wifi: map[string]bool{}}
		}
		s.name = strings.TrimSpace(f.GetFormItem(0).(*tview.InputField).GetText())
		s.title = f.GetFormItem(1).(*tview.InputField).GetText()
		s.init = strings.TrimSpace(f.GetFormItem(2).(*tview.InputField).GetText())
		at := e.selectedSpace() + 1
		e.cfg.spaces = append(e.cfg.spaces[:at], append([]*space{s}, e.cfg.spaces[at:]...)...)
		e.closeModal("add")
		e.changed()
		e.list.Select(at+1, 0)
	})
	f.AddButton("Cancel", func() { e.closeModal("add") })
	f.SetCancelFunc(func() { e.closeModal("add") })
	e.showModal("add", f, 66, 11)
}

func (e *editor) renameSpace(index int) {
	s := e.cfg.spaces[index]
	f := modalForm("Rename space " + s.name)
	f.AddInputField("Name", s.name, 31, nil, nil)
	f.AddTextView("", "Only the name changes; the title stays as it is.", 0, 1, false, false)
	done := func() {
		s.name = strings.TrimSpace(f.GetFormItem(0).(*tview.InputField).GetText())
		e.closeModal("rename")
		e.changed()
	}
	f.GetFormItem(0).(*tview.InputField).SetDoneFunc(func(key tcell.Key) {
		if key == tcell.KeyEnter {
			done()
		} else if key == tcell.KeyEscape {
			e.closeModal("rename")
		}
	})
	f.AddButton("Rename", done).AddButton("Cancel", func() { e.closeModal("rename") })
	f.SetCancelFunc(func() { e.closeModal("rename") })
	e.showModal("rename", f, 60, 8)
}

func (e *editor) removeSpace(index int) {
	s := e.cfg.spaces[index]
	f := modalForm("Remove space " + s.name)
	f.AddTextView("", "Type "+tview.Escape(s.name)+" to remove this space.", 0, 1, false, false)
	f.AddInputField("Name", "", 31, nil, nil)
	remove := func() {
		if f.GetFormItem(1).(*tview.InputField).GetText() != s.name {
			f.GetFormItem(0).(*tview.TextView).SetText(tag(colorError) + "The name does not match; type " + tview.Escape(s.name) + ".[-]")
			return
		}
		e.cfg.spaces = append(e.cfg.spaces[:index], e.cfg.spaces[index+1:]...)
		e.closeModal("remove")
		e.changed()
	}
	f.GetFormItem(0).(*tview.TextView).SetDynamicColors(true)
	f.GetFormItem(1).(*tview.InputField).SetDoneFunc(func(key tcell.Key) {
		if key == tcell.KeyEnter {
			remove()
		} else if key == tcell.KeyEscape {
			e.closeModal("remove")
		}
	})
	f.AddButton("Remove", remove).AddButton("Cancel", func() { e.closeModal("remove") })
	f.SetCancelFunc(func() { e.closeModal("remove") })
	f.SetFocus(1)
	e.showModal("remove", f, 64, 8)
}

func (e *editor) moveSpace(index, by int) {
	to := index + by
	if index < 0 || to < 0 || to >= len(e.cfg.spaces) {
		return
	}
	e.cfg.spaces[index], e.cfg.spaces[to] = e.cfg.spaces[to], e.cfg.spaces[index]
	e.changed()
	e.list.Select(to+1, 0)
}

func (e *editor) problemsModal() {
	list := tview.NewList().ShowSecondaryText(false).SetHighlightFullLine(true).
		SetSelectedBackgroundColor(colorSelect)
	list.SetBorder(true).SetTitle(" Problems: saving is blocked by errors ").SetTitleAlign(tview.AlignLeft)
	for _, p := range e.problems {
		p := p
		mark := tag(colorWarning) + "warning[-] "
		if p.severity == errorProblem {
			mark = tag(colorError) + "error[-]   "
		}
		list.AddItem(mark+tview.Escape(p.text), "", 0, func() {
			e.closeModal("problems")
			if p.space >= 0 {
				e.openSpace(p.space)
				e.showTab(p.tab)
			}
		})
	}
	list.SetDoneFunc(func() { e.closeModal("problems") })
	e.showModal("problems", list, 78, min(len(e.problems)+2, 20))
}

// review shows the problems or, when there are no errors, the diff to write.
func (e *editor) review() {
	if e.readOnly != nil {
		e.message("Read-only", "Saving is disabled: "+e.readOnly.Error())
		return
	}
	if countErrors(e.problems) > 0 {
		e.problemsModal()
		return
	}
	if !e.dirty {
		e.message("Nothing to save", "No changes since the file was loaded.")
		return
	}
	text := e.cfg.encode()
	var out strings.Builder
	for _, p := range e.problems {
		out.WriteString(tag(colorWarning) + "warning: " + tview.Escape(p.text) + "[-]\n")
	}
	if len(e.problems) > 0 {
		out.WriteString("\n")
	}
	for _, line := range diffText(string(e.loaded), text) {
		switch line.op {
		case '-':
			out.WriteString(tag(colorError) + "-" + tview.Escape(line.text) + "[-]\n")
		case '+':
			out.WriteString(tag(colorAdded) + "+" + tview.Escape(line.text) + "[-]\n")
		default:
			out.WriteString(tag(colorDim) + " " + tview.Escape(line.text) + "[-]\n")
		}
	}
	view := tview.NewTextView().SetDynamicColors(true).SetText(out.String())
	view.SetBorder(true).SetTitle(" Review: " + e.path + " ").SetTitleAlign(tview.AlignLeft)
	buttons := modalForm("")
	buttons.SetBorder(false)
	buttons.SetButtonsAlign(tview.AlignRight)
	note := textLine().SetText(" Applies at next boot. The current file is kept as " +
		tview.Escape(previousPath(e.path)) + ".")
	note.SetTextColor(colorDim)
	buttons.AddButton("Write", func() {
		e.closeModal("review")
		e.write(text)
	})
	buttons.AddButton("Back", func() { e.closeModal("review") })
	buttons.SetCancelFunc(func() { e.closeModal("review") })
	layout := tview.NewFlex().SetDirection(tview.FlexRow).
		AddItem(view, 0, 1, false).
		AddItem(note, 1, 0, false).
		AddItem(buttons, 3, 0, true)
	layout.SetInputCapture(func(event *tcell.EventKey) *tcell.EventKey {
		switch event.Key() {
		case tcell.KeyUp, tcell.KeyDown, tcell.KeyPgUp, tcell.KeyPgDn, tcell.KeyHome, tcell.KeyEnd:
			view.InputHandler()(event, nil)
			return nil
		case tcell.KeyEscape:
			e.closeModal("review")
			return nil
		}
		return event
	})
	e.pages.AddPage("review", layout, true, true)
}

func (e *editor) write(text string) {
	durability, err := saveConfig(e.path, e.loaded, text)
	if err != nil {
		e.message("Not saved", tview.Escape(err.Error()))
		return
	}
	e.loaded, e.dirty = []byte(text), false
	e.refresh()
	result := "Saved " + e.path + ". It applies at the next boot.\nPrevious: " + previousPath(e.path)
	if durability != nil {
		result += "\n\nSyncing the directory failed (" + durability.Error() + "); the file is in place but may not survive a crash until it is synced."
	}
	e.message("Saved", tview.Escape(result))
}

func (e *editor) replace(data []byte, loaded []byte, dirty bool) error {
	top, err := parseLiteral(string(data))
	if err != nil {
		return err
	}
	cfg, err := decodeConfig(top)
	if err != nil {
		return err
	}
	e.cfg, e.loaded, e.dirty = cfg, loaded, dirty
	e.pages.SwitchToPage("list")
	e.refresh()
	return nil
}

func (e *editor) discard() {
	confirm := func() {
		data, err := load(e.path)
		if err == nil {
			err = e.replace(data, data, false)
		}
		if err != nil {
			e.message("Cannot reload", tview.Escape(err.Error()))
		}
	}
	if !e.dirty {
		confirm()
		return
	}
	m := tview.NewModal().SetText("Discard all changes and reload " + e.path + "?").
		AddButtons([]string{"Discard", "Back"}).
		SetDoneFunc(func(_ int, label string) {
			e.pages.RemovePage("discard")
			if label == "Discard" {
				confirm()
			}
		})
	e.pages.AddPage("discard", m, true, true)
}

func (e *editor) loadPrevious() {
	path := previousPath(e.path)
	m := tview.NewModal().SetText("Open " + path + " as unsaved changes?\nSaving it makes it the configuration again.").
		AddButtons([]string{"Load", "Back"}).
		SetDoneFunc(func(_ int, label string) {
			e.pages.RemovePage("previous")
			if label != "Load" {
				return
			}
			data, err := load(path)
			if err == nil {
				err = e.replace(data, e.loaded, true)
			}
			if err != nil {
				e.message("Cannot load previous", tview.Escape(path+": "+err.Error()))
			}
		})
	e.pages.AddPage("previous", m, true, true)
}

func (e *editor) quit() {
	if !e.dirty {
		e.app.Stop()
		return
	}
	m := tview.NewModal().SetText("The configuration has unsaved changes.").
		AddButtons([]string{"Save", "Discard", "Back"}).
		SetDoneFunc(func(_ int, label string) {
			e.pages.RemovePage("quit")
			switch label {
			case "Save":
				e.review()
			case "Discard":
				e.app.Stop()
			}
		})
	e.pages.AddPage("quit", m, true, true)
}

const helpText = `Space list
  Up/Down, PgUp/PgDn  move          Enter   open the space
  a  add a space      c  copy       r       rename
  Del  remove (type the name)       [ ]     move in tab order

Space detail
  1-4  General, Settings, Roots, Grants      Esc   back to the list
  Enter  edit or toggle   Space  toggle      a/e/Del  add, edit, remove

Anywhere
  Ctrl+S  check, review the diff and save    Ctrl+R  discard changes
  Ctrl+P  open boot.lua.previous             Ctrl+Q, Ctrl+C  quit
  Esc closes a dialog. Changes apply at the next boot.`

func (e *editor) help() {
	view := tview.NewTextView().SetText(helpText)
	view.SetBorder(true).SetTitle(" Keys ").SetTitleAlign(tview.AlignLeft)
	view.SetInputCapture(func(*tcell.EventKey) *tcell.EventKey {
		e.closeModal("help")
		return nil
	})
	e.showModal("help", view, 78, 18)
}

// refresh revalidates and redraws whichever page is current.
func (e *editor) refresh() {
	e.problems = e.cfg.validate()
	e.refreshList()
	if e.space < len(e.cfg.spaces) {
		e.refreshDetail()
	}
}

// loadFailure shows why the file cannot be edited and exits on any key.
func loadFailure(path string, err error) error {
	setTheme()
	app := tview.NewApplication()
	view := tview.NewTextView().SetDynamicColors(true).SetText(
		"\n " + tag(colorError) + "Cannot edit " + tview.Escape(path) + "[-]\n\n " + tview.Escape(err.Error()) +
			"\n\n Fix the file by hand, or restore it, and check it with config validate." +
			"\n The rescue entry keeps system:// read-write, for example:\n" +
			"   cp system://config/boot.lua.previous system://config/boot.lua\n\n " +
			tag(colorDim) + "Press any key to exit.[-]")
	view.SetBorder(true).SetTitle(" Spaces ").SetTitleAlign(tview.AlignLeft)
	view.SetInputCapture(func(*tcell.EventKey) *tcell.EventKey {
		app.Stop()
		return nil
	})
	return app.SetRoot(view, true).Run()
}
