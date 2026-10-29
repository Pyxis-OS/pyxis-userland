# spaces

Optional first-party terminal editor for the spaces in an installed system's
`system://config/boot.lua`, using tview v0.42.0 and tcell v2.13.10. Changes
apply at the next boot.

```sh
spaces            # edits system://config/boot.lua
spaces PATH       # edits another file, such as a copy
```

It edits every space key that `libconfig/boot.c` reads, adds, copies, renames,
removes and reorders spaces, and edits roots and selected sources. Other
top-level settings and keys it does not know are kept unchanged; unknown keys
are listed as warnings. The file is rewritten in a fixed form without comments.

Saving checks a copy of the `boot.c` space rules, shows a diff, refuses if the
file changed since it was loaded, then keeps the loaded file as
`PATH.previous` and replaces `PATH`. Each staged file is synced before its
replacing rename, and the directory afterwards. Go on Pyxis cannot sync a
directory yet (its directory handles drop CREATE and REMOVE), so the saved
message names the `sync` command that does. The rule copy exists only
because Go on Pyxis cannot launch `config validate`; boot remains the
authority. A new space key changes `boot.c` and `validate.go`/`config.go`
together.

```sh
./space-editor/build.sh /absolute/pyxis-go-prefix /absolute/fresh/spaces-build
./space-editor/build.sh /absolute/pyxis-go-prefix /absolute/fresh/spaces-linux linux
```

The build follows [git](../git/README.md): dependencies download only through
the internal Go proxy with the committed module/checksum pins, ordered patches
apply to the private vendor tree and compilation is offline. The first tcell
patch adds the `pyxis` terminal profile on every platform, so the Linux build
draws with the same sequences. The second adds tcell's Pyxis terminal on the
supplied stdin/stdout: native raw mode and size, Enter's LF read as CR, and the
pyxis-go `WaitTerminalResized` wait. That waiter ends only when stdin closes, so
it lives until process exit; Stop and Start switch its callback off and on.
Pyxis stages at
`OUT/stage/bin/spaces.pxb`, Linux at `OUT/stage-linux/bin/spaces`, with complete
notices for compiled modules, patches, source hashes and compiler revision.

The manifest requests only memory, threads, precise clock and random. The file
is reached through the caller's inherited `system://` root; read-only access
opens the editor with saving disabled.
