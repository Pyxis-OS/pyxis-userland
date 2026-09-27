# Pyxis userland

Freestanding C runtime, native and terminal libraries, applications and boot
scripts for Pyxis OS. Requires GNU Make, the prebuilt
`x86_64-unknown-pyxis-` compiler and a selected Pyxis SDK. The session launcher
also needs the Lua development files exported by the ports build.

```sh
make SDK=/path/to/sdk LUA_PREFIX=/path/to/ports-dev/lua
make SDK=/path/to/sdk hello client server
make -f runtime.mk SDK=/path/to/sdk      # runtime in build/runtime
make install SDK=/path/to/sdk LUA_PREFIX=/path/to/ports-dev/lua DESTDIR=/path/to/guest-tree
make clean
make -f runtime.mk clean
```

`SDK` defaults to `build/sdk`; `BUILD` overrides the output directory for either
phase. `LUA_PREFIX` defaults to `build/ports-dev/lua` and supplies `include` and
`lib/liblua.a` for `session`; the SDK/runtime has no Lua dependency. `install` recreates a payload tree of selected programs, init and assets;
it excludes objects/debug ELFs and removes stale installed files. Application builds consume the complete SDK. Runtime builds use local
runtime headers, exported Pyxis ABI/format headers, build settings and the
SDK's shared shebang source. TLSF is vendored locally with its license and pin.

The Pyxis parent repository pins this repository as its `userspace` submodule
and orchestrates header export, runtime build, SDK assembly, application build
and boot-image assembly. Use its `make sdk`, `make image` and `make run` targets
for the integrated build. Runtime changes reach applications after SDK assembly.
See [import provenance](IMPORT.md) for the original history and dependency split.

From the interactive shell, `session app://server.pxe` runs the endpoint example.
The server creates its receiver, launches two clients with caller grants, receives
both requests and replies in reverse order. `session app://server.pxe --wide`
uses 4 KiB requests and replies with four file grants in each direction;
`session app://server.pxe --abandon` closes one receipt so its caller sees
abandonment. `--saturate` retains sixteen receipts while a seventeenth client
reports queue saturation, then replies in reverse order. `--close` closes the
receiver after receiving a call and starts another client against a retained
caller grant; `--exit` lets process teardown close the receiver. The shell
delegates endpoint creation and launch authority only to `session` commands.

`session app://server.pxe --send` grants a client send-only authority. It sends
4 KiB and four file grants, closes its sources and exits before the server
receives the message. The server finishes the receipt and then reads the
retained file grants. `--mixed` holds one call receipt while a send-only client
admits fifteen sends and gets `QUEUE_FULL` on its sixteenth. The server waits
for that sender to exit, receives and finishes the sends, then replies to the
held call.

Deadline examples use the session's explicit monotonic clock grant. `--expired`
rejects a past deadline before admission. `--queued-timeout` leaves a call in
the queue until its deadline and confirms that a later send is received first.
`--deadline-reply` completes a call before its deadline. `--delivered-timeout`
blocks in RECEIVE for cancellation, rejects a late reply without consuming its
receipt, then finishes it. `--cancel-full` holds one received call and fifteen
queued sends through expiry, confirming that the cancellation notice arrives
before the ordinary queue despite all sixteen slots being occupied. Admission
remains full until the provider finishes the canceled receipt.
`--cancel-finish` closes an expired receipt before RECEIVE and confirms that
its pending notice is removed.

`session app://counter.pxe` runs a provider with two counter exports on one
receiver. A launched client receives different resource grants, attenuates a
copy and an IPC attachment, verifies protocol matching, and uses both CALL and
send-only delivery. The provider authenticates object ID and actual rights,
then acknowledges natural retirement. `session app://counter.pxe --withdraw`
withdraws an export during a delivered call, finishes its cancellation receipt,
acknowledges retirement while an old client handle remains open, and reuses the
same ID for a new export. `--exit` ends the provider with a delivered call; its
client reports closure after owner-process teardown.
`--queued-withdraw` parks the provider after the client sends a marker through
its export, then withdraws while the client's following CALL waits in the queue;
the client reports closure before delivery. `--retire-full` fills all sixteen
ordinary delivery slots with raw SENDs, withdraws an idle export, receives its
retirement notice ahead of those SENDs, acknowledges it and drains them.

The init script creates a namespace before handing off to the interactive
session. The shell keeps management authority; ordinary children receive
LOOKUP only. A provider launched through `service start` gets an endpoint
creation grant and a one-use publication CALL endpoint, without a namespace
grant. The shell publishes its attached export and replies before the provider
starts serving. For example:

```sh
service start clicks app://counter.pxe --provide
counter --lookup clicks
counter --add clicks
counter --restrict clicks
counter --hold clicks &
service replace clicks app://counter.pxe --provide
counter --lookup clicks
service replace clicks app://counter.pxe --provide-once
counter --lookup clicks
counter --lookup clicks
namespace remove clicks
counter --lookup clicks
```

The first lookup prints 4, ADD prints 7, and `--restrict` reports denied
namespace removal and denied ADD. The held client later prints 7 from the old
provider; a fresh lookup after replacement prints 4. `--provide-once` answers
one lookup and exits. The following lookup reports ENDPOINT_CLOSED, and after
removal it reports NOT_FOUND.

`service start` requires an absent binding; `service replace` requires an
existing one, including a dead binding. Both reject names that also select a
filesystem root. The provider's publication CALL expires after ten seconds,
but the shell can remain waiting if a provider exits before sending its CALL:
RECEIVE cannot wait on provider exit at the same time. `namespace create`
selects a fresh namespace, also when the shell already has one. In a trusted
interactive shell, the following creates a separately populated namespace;
new ordinary children cannot see `first`, while the old namespace remains
held by the previous session until its owners exit:

```sh
service start first app://counter.pxe --provide
namespace create
counter --lookup first
service start second app://counter.pxe --provide
counter --lookup second
```

The first lookup reports NOT_FOUND; the second prints 4.

`namespace remove NAME` releases a binding without closing client grants
already looked up elsewhere.

The compiler must include the Pyxis x87/SSE2 defaults and floating-point libgcc
helpers. `mandelbrot` draws through a mapped display buffer using double
arithmetic. Hold arrows to pan, `=`/`+` and `-` to zoom, and Escape to return to
the TTY. It needs display, keyboard and clock grants. Libc supports floating-point
formatting and a small math subset; it does not provide a full libm.
