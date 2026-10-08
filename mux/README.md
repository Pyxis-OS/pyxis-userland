# Native terminal multiplexer

Build with `make mux SDK=/path/to/sdk` using the current Pyxis SDK. The SDK
must include attachment-authorized terminal resize and the general 32-interest
readiness bound. No compiler-container rebuild is required.

Trusted session startup selects `mux.pxe` only for a local space whose boot
configuration sets `multiplexer = true`; ordinary launch does not supply its
terminal creation or execution-group creation authority. Optional
`--tab-width 1..32` sets the initial spacing of each pane.

Controls and lifetime behavior are documented in Pyxis
[`docs/userland/multiplexer.md`](https://git.internal/PyxisOS/pyxis-os/src/branch/main/docs/userland/multiplexer.md).

`session.c` copies selected roots, cwd, environment and explicit local resources;
its returned attachment, group supervisor and root observer are owned by the
pane. `emulator.c` owns live cells and a bounded history ring; output parsing
allocates nothing. `layout.c` owns the eight-leaf split tree independently of
presentation. `main.c` owns input routing, readiness and lifecycle; `render.c`
composes colored cells through the borrowed outer CONSOLE handles.
