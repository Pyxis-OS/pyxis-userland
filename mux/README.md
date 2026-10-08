# Native terminal multiplexer

Build with `make mux SDK=/path/to/sdk` using the current Pyxis SDK. The SDK
must include attachment-authorized terminal resize, the general 32-interest
readiness bound, terminal pointer control and view identities.
No compiler-container rebuild is required.

Trusted session startup selects `mux.pxe` only for a local space whose boot
configuration sets `multiplexer = true`; ordinary launch does not supply its
terminal creation or execution-group creation authority. Optional
`--tab-width 1..32` sets the initial spacing of each pane.
Local mux receives a separate `terminal_pointer` grant through trusted session
startup. Pane children receive no controller; their existing graphics pointer
grants remain independent. Remote mux retains keyboard controls.

Left dragging selects visible pane text; content and heading clicks focus a
pane. Wheel over content moves that pane's history by three rows per detent
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
composes colored cells through the borrowed outer CONSOLE handles.
