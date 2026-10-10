# Native terminal multiplexer

Build with `make mux SDK=/path/to/sdk` using the current Pyxis SDK. The SDK
must include attachment-authorized terminal resize, the general 32-interest
readiness bound, terminal pointer control, view identities and the shared
terminal style header/source.
No compiler-container rebuild is required.

Trusted session startup selects `mux.pxe` only for a local space whose boot
configuration sets `multiplexer = true`; ordinary launch does not supply its
terminal creation or execution-group creation authority. Optional
`--tab-width 1..32` sets the initial spacing of each pane.
Local mux receives a separate `terminal_pointer` grant through trusted session
startup. Pane children receive no controller; their existing graphics pointer
grants remain independent. Remote mux retains keyboard controls.

Left dragging selects visible pane text only after crossing into a different
cell while held. A press clears the old selection; a click or movement within
the starting cell leaves none. Content and heading clicks focus a pane. Wheel over content moves that pane's history by three rows per detent
without changing keyboard focus, returning to live input at the newest row.
Layout, resize and explicit history movement clear selection. Unrelated output
and color changes preserve it; selected glyph mutation, eviction or movement
outside the visible view clears it. Selection is a colored overlay, with no
clipboard publication or Copy command.

Controls and lifetime behavior are documented in Pyxis
[`docs/userland/multiplexer.md`](https://git.internal/PyxisOS/pyxis-os/src/branch/main/docs/userland/multiplexer.md).

`session.c` copies selected roots, cwd, environment and explicit local resources;
its returned attachment, group supervisor and root observer are owned by the
pane. `emulator.c` owns live cells and a bounded history ring; output parsing
allocates nothing. `layout.c` owns the eight-leaf split tree independently of
presentation. `pointer.c` owns spatial hit testing and view barriers.
`main.c` owns input routing, readiness and lifecycle; `render.c`
composes styled cells through the borrowed outer CONSOLE handles.

Pane cells and the 1,024-row history retain tagged default, indexed and RGB
colors, plus bold, italic, underline and reverse. Indexed colors 0–15 remain
indices until the outer TTY resolves its active palette. SGR supports 1/22,
3/23, 4/24, 7/27, ordinary/bright palette colors, defaults, `38/48;5;n` and
semicolon `38/48;2;r;g;b` with 0–255 components. The shared parser applies a
complete SGR atomically, with at most 16 parameters; malformed extended colors,
colon forms and oversized sequences preserve the current style. Erased or
scrolled-in blanks keep current colors with no attributes. Saved cursors,
alternate screens and resize copies retain styles; selection changes only
presentation colors and clears reverse, preserving the other attributes.
