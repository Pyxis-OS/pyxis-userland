# Pyxis userland

Freestanding C runtime, native and terminal libraries, applications and boot
scripts for Pyxis OS. Requires GNU Make, the prebuilt
`x86_64-unknown-pyxis-` compiler and a selected Pyxis SDK. The session launcher
also needs the Lua development files exported by the ports build; the HTTP
provider needs the picohttpparser and Mbed TLS development exports. Host Lua 5.4 (`LUA`, default
`lua`) generates the packaged [iobench fixture](iobench/README.md).

```sh
make SDK=/path/to/sdk LUA_PREFIX=/path/to/ports-dev/lua PICOHTTPPARSER_PREFIX=/path/to/ports-dev/picohttpparser MBEDTLS_PREFIX=/path/to/ports-dev/mbedtls
make SDK=/path/to/sdk hello client server
make -f runtime.mk SDK=/path/to/sdk      # runtime in build/runtime
make install SDK=/path/to/sdk LUA_PREFIX=/path/to/ports-dev/lua PICOHTTPPARSER_PREFIX=/path/to/ports-dev/picohttpparser MBEDTLS_PREFIX=/path/to/ports-dev/mbedtls DESTDIR=/path/to/guest-tree
make clean
make -f runtime.mk clean
```

`SDK` defaults to `build/sdk`; `BUILD` overrides the output directory for either
phase. `LUA_PREFIX` defaults to `build/ports-dev/lua` and supplies `include` and
`lib/liblua.a` for `session`. `PICOHTTPPARSER_PREFIX` defaults to
`build/ports-dev/picohttpparser` for `libhttp` and `httpfs`; the SDK/runtime is
independent of these application libraries. `MBEDTLS_PREFIX` defaults to
`build/ports-dev/mbedtls`; `make libtls` builds `BUILD/libtls.a`. Its native
authority, trust and ownership contract is in [libtls/tls.h](libtls/tls.h).
Applications link this archive with the export's `MBEDTLS_LIBRARIES` before SDK
libraries; compile Mbed TLS consumers with `MBEDTLS_CPPFLAGS` from its
`share/mbedtls.mk`. `install` recreates a payload tree of selected programs, init and assets;
it excludes objects/debug ELFs and removes stale installed files. Application builds consume the complete SDK. Runtime builds use local
runtime headers, exported Pyxis ABI/format headers, build settings and the
SDK's shared shebang source. TLSF is vendored locally with its license and pin.

The [native installer](installer/README.md) consumes the SDK's pinned npfs
codecs and explicit install-mode disk/source grants. It is packaged normally;
`init-installed` mounts its persistent `system://` volume while retaining a RAM home.

`make libhttp` builds `BUILD/libhttp.a`. Consumers link it with picohttpparser,
libtls and `MBEDTLS_LIBRARIES` before SDK libraries. Each fetch receives an
explicit HTTP or HTTPS client; HTTPS borrows a ready libtls runtime and verifies
the URI's DNS hostname. It accepts authenticated TLS EOF for close-delimited
bodies and returns structured fetch/cleanup diagnostics. Authority, body-storage
ownership and result mappings are described in [libhttp/http.h](libhttp/http.h).
The library loads no trust or startup resources. `httpfs --https` owns its ready
TLS runtime and loads the packaged public roots before publication.

The Pyxis parent repository pins this repository as its `userspace` submodule
and orchestrates header export, runtime build, SDK assembly, application build
and boot-image assembly. Use its `make sdk`, `make image` and `make run` targets
for the integrated build. Runtime changes reach applications after SDK assembly.
See [import provenance](IMPORT.md) for the original history and dependency split.

[system_info.h](include/system_info.h) queries the running kernel's identity,
guest CPU and global allocator memory through a borrowed READ grant. Failed
queries preserve caller output. Init scripts, sessions and local/remote shells
forward `system_info` to ordinary children when supplied; provider launches and
restricted launchers may omit it. Applications must treat a missing grant as
unavailable information. Memory is allocator capacity, labeled **Memory (allocator)**.

[lsusb](lsusb/README.md) lists the immutable boot USB inventory with controller
state, physical port paths through hubs, speed, numeric IDs and checked interfaces.
It uses the same borrowed `system_info` READ grant and optional packaged USB ID names.

[launcher.h](include/launcher.h) provides execution-group creation, sealing,
termination and completion observation. `launcher_create_group` requires
CREATE_GROUP authority and returns a supervision grant with CONTROL|WAIT plus a
group-bound launcher with LAUNCH only. It preserves the caller's reply on failure.
Closing the final CONTROL grant requests termination. `execution_group_seal`
closes admission idempotently while existing members continue running.
`execution_group_terminate` requests stopping members at safe kernel boundaries;
success acknowledges the request. `execution_group_wait` requires WAIT and returns
when sealed admission, member reclamation and group-owned cleanup finish. Completion
is repeatable and reports no aggregate program success. WAIT-only observers do not
retain controlling supervision; open empty groups are not complete. These helpers
report `CALL_OUTCOME_UNKNOWN` for an untrustworthy response; creation must not be
retried automatically. `wait_many` observes WAIT_COMPLETE on group WAIT grants and
can combine them with TCP and terminal attachment interests.

[terminal.h](include/terminal.h) provides native terminal creation and attachment
queue helpers. Applications use the returned CONSOLE input/output grants through
libterm or libc streams. Input EOF returns `TERM_KEY_EOF` through key decoding and
`TERM_LINE_EOF` through line editing, discarding any partial line. Ctrl+D retains
the line editor's empty-line-only behavior.

For manually invoked performance measurements, see [iobench](iobench/README.md)
for files and pipes and [ipcbench](ipcbench/README.md) for CALL/SEND.

`session.pxe --configure-network` applies `config/network.lua`. A `net0` table
requires exactly one selector: `driver = "virtio"` or a locally supplied `mac`
string containing six colon-separated hex pairs for a nonzero unicast address.
The packaged configuration selects VirtIO. A unique match binds until reboot;
there is no fallback to another controller. Missing `net0` preserves settings;
`net0 = false` clears IPv4 settings while retaining the binding. Remote startup
looks up the configured candidate without binding it, then waits for the setup
owner to bind and assign an address.

`net0 = { driver = "virtio", dhcp = true }` requests a lease instead of static
address/prefix/gateway fields; the packaged profile uses this form. Acquisition
waits at most about ten seconds, then starts the local session offline on failure.
Task 2 keeps a successful lease until reboot; reboot before lease expiry until
maintenance is implemented. Explicit profile `dns.server` wins, then first lease
DNS, then `1.1.1.1`. Launchers read the chosen DNS for each new program; existing
programs retain their startup environment. Ordinary shells receive only
NET_CONFIG READ and UDP OPEN, with no configuration/broadcast write authority.

`session.pxe --configure-network --tcp-server ADDRESS PORT [--tcp-count COUNT]`
hands off to a TCP echo server instead of the local shell. It needs
explicit TCP LISTEN authority from native init or a trusted session handoff;
normal interactive sessions retain only CONNECT authority. The launcher creates
the exact bound listener, then gives `tcp --serve [COUNT]` only that listener,
memory, a readable clock and stdout/stderr. It grants no listening service,
launcher or filesystem roots. Each connection echoes until peer EOF, then shuts
down its writes and closes the stream. A single readiness loop services up to
four clients with 4 KiB of pending output each and bounded work per client.
Reads pause when output is full; writable readiness is watched only with pending
output. Connection errors close that client and preserve service to the others.
With a positive COUNT it stops admission after that many accepted connections,
finishes active streams and exits unsuccessfully if any connection failed;
otherwise it keeps serving. Wait timeouts resume waiting without expiring clients.

From the interactive shell, `session app://server.pxe` runs the endpoint example.
The server creates its receiver, launches two clients with caller grants, receives
both requests and replies in reverse order. `session app://server.pxe --wide`
uses 4 KiB requests and replies with four file grants in each direction;
`session app://server.pxe --abandon` closes one receipt so its caller sees
abandonment. `--saturate` retains sixteen receipts while a seventeenth client
reports queue saturation, then replies in reverse order. `--close` closes the
receiver after receiving a call and starts another client against a retained
caller grant; `--exit` lets process teardown close the receiver. The shell
delegates launch authority through `session`; endpoint creation is also
delegated to providers launched with `service start` or `service replace`.

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

`app://init-install.pxe` is the dedicated trusted installer handoff. It requires
the install entry's `disks` inventory service and read-only `boot_kernel` and
`boot_archive` FILE grants, opens `app://installer.pxe`, and launches only that
native program. The child receives these resources, memory, console input/output,
a readable clock, random bytes and the read-only app root. It receives no launcher,
home root or generic session handoff. Standard streams use separate grants.
Missing authority or an unpackaged installer reports failure and stops; this
entry does not supply an installer stub. Init waits for completion and reports
its status.

[disk.h](include/disk.h) provides bounded native inventory, exclusive raw-disk
I/O, flush/release and read-only volume-open wrappers. Raw writes require a live
claim; flush supplies durability and release rescans GPT. Device alignment and
target bounds remain native checks. No wrapper retries an uncertain operation.

Trusted init selects a native volume with
`mount --partition 1 --volume system --read-only data://`, or requests writable
content grants with `--read-write`. Exactly one access mode is required. Writable
roots require WRITE on the supplied `native_mount` authority and a writable
backend with understood filesystem features. `--optional` continues only when
`native_mount` is absent; failures from supplied authority stop the script.
Names are explicit directory bindings, separate from service namespace entries. Duplicate or conflicting names fail without replacement.
The shell reserves binding storage before opening and rechecks the service
namespace before publication; it closes an unpublished root on conflict.
The profile supports sixteen selected roots including app, home and HOST.
Default scripts require no native disk; HOST keeps its existing command syntax.
Native mounts request filesystem observation when the supplied mount authority
holds OBSERVE; `--no-info` explicitly omits it. The option applies only to native
mounts. A mount authority without OBSERVE can still mount content.

In a trusted shell holding `native_mount`, `sync --disk` synchronizes dirty data
and metadata in every mounted native pool on that authority's configured disk.
It requires WRITE and takes no disk selector. Completion means the required
transactions are committed; background checkpointing can remain. A failure may
follow durable commits in other pools. The shell performs this operation itself
and does not delegate mount authority to an external command. Ordinary sessions
have no mount grant and report unavailable. Existing `sync path...` continues to
synchronize the named files or directories. Closing a handle does not sync it.

[directory.h](include/directory.h) provides `directory_filesystem_info` through a
borrowed FILESYSTEM_INFO directory grant. Its bounded record supplies npfs
type, read-only and GPT/filesystem degraded flags, opaque pool/volume IDs,
volume name, retained selected generation and shared-pool allocatable capacity
in bytes. Capacity excludes the two superblock slots but includes metadata and
reserves; equal pool IDs identify shared capacity that must not be summed per
binding or volume. Opening validates geometry and root envelopes, not global
allocation accounting. All record fields are available on success; used/free
bytes, charged bytes, guarantees, quotas and percentages are unavailable and
never implied zero. Queries preserve caller output on failure, perform no
whole-image check and acquire no additional authority. Other directory backends
return BAD_OPERATION.

Shell, session and remote handoffs forward the selected root list with queried
rights and transport masks. Providers requested with `--read-only` attenuate
both roots and cwd.
Read-only attenuation preserves held FILESYSTEM_INFO authority, including for
the HTTPS provider's native trust roots, without adding it to other grants.
The cwd chain is an explicit selection too: a launcher
selecting fewer roots must omit inherited cwd or select a safe cwd from that
subset. Display paths confer no authority. Mount resources remain in trusted
init and are excluded from child handoffs.

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
Script interpreter paths also resolve against the shell's current namespace
after a switch, so a name bound as both service and filesystem root is rejected.

`namespace remove NAME` releases a binding without closing client grants
already looked up elsewhere.

The compiler must include the Pyxis x87/SSE2 defaults and floating-point libgcc
helpers. `mandelbrot` draws through a mapped display buffer using double
arithmetic. Hold arrows to pan, `=`/`+` and `-` to zoom, and Escape to return to
the TTY. It needs display, keyboard and clock grants. Libc supports floating-point
formatting and a small math subset; it does not provide a full libm.

`httpfs` publishes read-only HTTP snapshots; `httpfs --https` selects HTTPS.
Configured boot sessions use `--start-services` to publish independent `http`
and `https` instances after selecting `DNS_SERVER`. HTTPS loads
`app://share/ca-certificates/cacert.pem` before publication, with bounded entropy
initialization. Reported HTTPS setup failure leaves HTTP and local startup usable.
To augment public roots, restart or replace the HTTPS instance:

```
service replace --read-only https app://httpfs.pxe --https --ca-bundle home://private-ca.pem
```

`--read-only` attenuates the launched provider's native filesystem roots and
working directories. HTTPS accepts only this read-only native trust authority
and receives no namespace. `service start --optional --read-only` continues only
after an acknowledged setup failure with successful cleanup. Unexpected crashes
before reporting can still leave service startup waiting.

Each instance owns immutable trust and independent snapshot budgets. Existing
snapshots remain readable after replacement until their grants retire. Publication
itself does not fetch remotely. With networking enabled, ordinary consumers can run
`cat http://example.com/` or
`cat http://example.com/ | tee home://example.html | cksum`.
Each open fetches independently under a 30-second budget, capped by any native
caller's earlier deadline. Retained snapshots share a 64 MiB storage account and
63 export slots. Build the provider with
`PICOHTTPPARSER_PREFIX=/path/to/ports-dev/picohttpparser`.

## License

Original Pyxis material is licensed under [MPL-2.0](LICENSE). See
[LICENSING.md](LICENSING.md) for scope and third-party exceptions.
