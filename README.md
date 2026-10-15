# Pyxis userland

Freestanding C runtime, native and terminal libraries, applications and boot
scripts for Pyxis OS. Requires GNU Make, the prebuilt
`x86_64-unknown-pyxis-` compiler and a selected Pyxis SDK. The session launcher
also needs the Lua development files exported by the ports build; the HTTP
provider needs the picohttpparser and Mbed TLS development exports. The
[xfer](xfer/README.md) terminal file-transfer program also uses the Mbed TLS
export for mandatory SHA-256 verification. Host Lua 5.4 (`LUA`, default
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
SDK's shared shebang, US key layout, terminal style and UTF-8 text sources. TLSF is vendored locally with its license and pin.

Shell and Lua launch lookup share `program_open` from [program.h](include/program.h).
A bare name selects `bin://NAME.pxe`, then the default entry of `bin://NAME.pxb`,
then the explicitly configured `PYXIS_BUNDLE_CATALOG`, then the rescue
`boot://NAME.pxe`. Only absence permits fallback; a broken or denied candidate
fails. Direct hits do not read the catalog or enumerate bin. Explicit `.pxb`
paths use the same manifest, read-only app/resource roots, bounded stack and
actual-grant preparation; plain FILE paths keep ordinary launch behavior.
Logical `bin://NAME` aliases retain literal FILE lookup before the same bin
candidates and catalog, without rescue fallback. ZIP launch remains unavailable.

`tree` uses UTF-8 box-drawing branches for terminal, pipe and file output.
Use `tree --ascii` for byte-oriented consumers such as `less`, whose line wrapping
can split a UTF-8 branch. `--ascii` selects ASCII branches and continuation
prefixes; filenames keep the existing output behavior.

The [native installer](installer/README.md) consumes the SDK's pinned npfs
codecs and explicit install-mode disk/source grants. It is packaged normally;
`init-installed` receives the persistent `system://` and `home://` volumes from
boot init while retaining RAM `tmp://`.

The dedicated [native FAT32 qualification bootstrap](init-fat32-mount/README.md)
mounts an explicit scratch GPT selection or the trusted boot-ESP selection
read-only, delegates an `esp` root to direct ordinary `ls`/`cat` children and
explicitly retires the mount after closing data views. It is separate from raw
inspection/writing. Its install-only `init-fat32-mount-write.pxe` alias selects/releases a
RW scratch disk and provides native mutation, held-FILE and publication-interval
commands; ordinary shell/ls/cat children retain read-only roots. It supplies no
installed shell binding or editor workflow.

`boot://boot-init.pxe` is the boot init the kernel starts in Caelum's space. It
reads `config/live.lua`, or `config/installed.lua` plus the pool override
`system://config/boot.lua` on installed boots, mounts each configured volume
once and creates each space with its init through the `space_factory`
resource, forwarding only the configured roots and, to the one network owner,
network WRITE authority. Spaces start in `home://` unless their `start` names
another root. Live boots make `home://` a RAM volume. If no space starts it
creates a `rescue` shell space.
The archive's `hostname` defaults to `pyxis`. Installed boots also read
`system://config/machine/network/hostname`, including rescue boots; a missing,
unreadable or invalid key reports the reason and keeps the archive default.
Names are 1–63 ASCII letters, digits and internal hyphens, preserving case;
the stored key accepts one trailing LF or none. Boot init sets the kernel's
name once before creating spaces. `system://config/boot.lua` cannot set it.
The archive's `input` table selects `mouse_scroll_direction` and
`touchpad_scroll_direction`, each `natural` or `traditional` (natural by default).
Installed boots read `system://config/machine/input/mouse/scroll-direction` and
`system://config/machine/input/touchpad/scroll-direction`, accepting the exact
lowercase value with one trailing LF or none. Missing, unreadable or invalid
keys keep their respective archive defaults. `input.diagnostics = true` in the
archive enables fallback reports; it defaults false. Stock boot init installs
both directions once before starting spaces, including rescue. The `input_policy`
grant stays in boot init. `system://config/boot.lua` cannot override input policy.
Store changes apply next boot; explicitly sync
edited keys before reboot. No settings writer or live policy update is provided.
`clipboard_local` and `clipboard_shared` independently request the named
clipboard grants. Stock local interactive spaces enable both; remote startup
enables neither. An omitted setting retains the existing multiplexer default,
while an explicit false withholds that layer.
See [boot configuration](https://git.internal/PyxisOS/pyxis-os/src/branch/main/docs/userland/init.md#boot-configuration).

`make libhttp` builds `BUILD/libhttp.a`. Consumers link it with picohttpparser,
libtls and `MBEDTLS_LIBRARIES` before SDK libraries. Each fetch receives an
explicit HTTP or HTTPS client; HTTPS borrows a ready libtls runtime and verifies
the URI's DNS hostname. It accepts authenticated TLS EOF for close-delimited
bodies and returns structured fetch/cleanup diagnostics. Authority, body-storage
ownership and result mappings are described in [libhttp/http.h](libhttp/http.h).
The library loads no trust or startup resources. `httpfs --https` owns its ready
TLS runtime and loads the packaged public roots before publication.

Kernel option `remote.beacon=NAME` selects reverse mode for the configured
`remote` space. Boot init forwards `PYXIS_REMOTE_BEACON` and a separate
BROADCAST-only `udp_beacons` grant through the trusted session bootstrap.
The boot option selects the name; the environment variable only carries it
between trusted launches and session configuration cannot create or replace it.
The daemon listens for matching UDP beacons on port 2324, closes discovery
before connecting to the sender, and resumes discovery after the old session
group finishes cleanup. Remote shells receive ordinary UDP OPEN authority
and neither the discovery grant nor its environment variable. Packaged live Remote
enables `launch = true`: foreground children receive LAUNCH bound to their session
or mux pane group, so descendants remain covered by its cleanup. Installed defaults
have no Remote space; owner-added entries can opt in with the same setting. Without the
option, the existing four-client TCP listener remains available.

The remote server selects `shell --remote-prompt` for its normal root shell.
An explicit `remote_multiplexer = true` Remote-space setting additionally allows
interactive `pyxis-remote --mux`. The daemon creates the outer mux and brokers
up to eight independent pane groups, retaining cleanup supervision. At most two
mux connections share the existing four connection slots. Mux and pane shells
retain the remote allowlist, with no physical pointer or clipboard grants.
Host character-window resize is negotiated only for `--mux`, within 512×256;
explicit client dimensions are ceilings. Tiny windows clip retained panes and
growth restores the layout. Native queued bytes and partial CSI/UTF-8 remain in
order; applied outer geometry does not acknowledge pane redraw or host display.
Keyboard controls, Pyxis colours and UTF-8 work, while stock `xfer` refuses and
prompt/drop automation is disabled.
Packaged live Remote enables `remote_multiplexer`; installed defaults have no
Remote space. Plain clients still select the shell. Use it only on a trusted
network: any peer reaching the unauthenticated service can request panes
with that space's existing filesystem and program grants.

Its opt-in `term_read_line_marked` editor writes OSC 133;B after drawing each
empty prompt, before reading input. Quiet shells and ordinary callers keep their
existing output behavior; shell children do not inherit this option.

Libterm line editing retains the complete prompt, text and cursor across live
resize. A line larger than the screen uses a visible window around the cursor;
the buffer capacity, including its NUL, controls how much text can be edited.
Adaptive reads need the startup `clock` grant with READ authority, supplied by
the stock shell profiles. Without that authority ordinary key reads still work
with the initially queried dimensions. Typed resize events preserve partial
Escape/CSI sequences and their original per-byte deadline.

Stock line readers opt into native safe paste while editing: LF/Tab become
spaces, paste never submits, and cancellation retains the inserted prefix.
Ctrl+Shift+C/V uses the space-local clipboard; Super+Shift+C/V uses the shared
one. Mux copies its completed selection and pastes into its focused live pane;
pending input refuses paste. Local sessions forward separate clipboard grants
through shells and mux pane shells to foreground launches. These store grants
give pane shells no terminal CONTROL; background/service/remote launches receive
neither. [clipboard.h](include/clipboard.h) also provides graphics publish/read/
has/refuse helpers, requiring matching physical activation and current display
and keyboard ownership. Raw readers do not accept paste.
A mux paste can cancel when its output scrolls visible rows, advancing the view
identity; the inserted prefix stays editable and is never submitted.

The Pyxis parent repository pins this repository as its `userspace` submodule
and orchestrates header export, runtime build, SDK assembly, application build
and boot-image assembly. Use its `make sdk`, `make image` and `make run` targets
for the integrated build. Runtime changes reach applications after SDK assembly.
See [import provenance](IMPORT.md) for the original history and dependency split.

[system_info.h](include/system_info.h) queries the running kernel's identity,
hostname, guest CPU and global allocator memory through a borrowed READ grant. Failed
queries preserve caller output. Init scripts, sessions and local/remote shells
forward `system_info` to ordinary children when supplied; provider launches and
restricted launchers may omit it. Applications must treat a missing grant as
unavailable information. Memory is allocator capacity, labeled **Memory (allocator)**.
`hostname` prints the boot-selected name and rejects arguments. Libc
`gethostname()` uses the same startup grant, requires room for the complete name
and NUL, and returns `ENAMETOOLONG` without changing the buffer when too small.
There is no runtime rename; an installed key change applies at the next boot.

[log.h](include/log.h) snapshots and reads retained kernel lines through a borrowed
read-only `log` grant, preserving caller output on failure. Each read returns
validated records, one line piece and its level each; `log_next_record` walks them.
Each reader owns its cursor; reads never consume another reader's text. `log`
prints through its initial snapshot end and exits, starting each line with its
level letter: `E`, `W`, `I`, `D` or `T`. `log -l LEVEL` prints only that level and
more severe ones (error, warn, info, debug, trace). `log -f` prints retained history,
then polls every 100 ms using the clock service; Ctrl+C uses the shell's foreground
stop behavior. Both report overwritten lines on stderr. Boot init forwards the grant to every space;
sessions and local/remote shells forward it to ordinary children when supplied.
Provider launches and restricted launchers may omit it. The ring is volatile;
panic capture remains best effort.

[lsusb](lsusb/README.md) lists the immutable boot USB inventory with controller
state, physical port paths through hubs, speed, numeric IDs and checked interfaces.
It uses the same borrowed `system_info` READ grant and optional packaged USB ID names.

[launcher.h](include/launcher.h) provides execution-group creation, sealing,
termination and completion observation. `launcher_create_group` requires LAUNCH.
An unbound launcher creates a local root for an ungrouped caller; a bound
launcher creates a subgroup within its binding. Members may select their own
or descendant bindings. The creator stays outside the new group. Its atomic
reply contains CONTROL|WAIT supervision plus a group/space-bound LAUNCH launcher,
with no CPU binding. It preserves the caller's reply on failure.
Closing the final CONTROL grant requests subtree termination.
`execution_group_seal` closes admission throughout the subtree idempotently
while existing members and subgroups continue running.
`execution_group_terminate` requests stopping members at safe kernel boundaries;
success acknowledges the request. `execution_group_wait` requires WAIT and returns
when sealed admission, member reclamation, subgroup completion and attributed
cleanup finish. Storage-only handles may outlive completion. Completion is
repeatable and reports no aggregate program success. WAIT-only observers do not
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
For manual native sibling qualification, see [threadprobe](threadprobe/README.md).

`session.pxe --configure-network` applies `config/network.lua`. A `net0` table
requires exactly one selector: `driver = "virtio"` or a locally supplied `mac`
string containing six colon-separated hex pairs for a nonzero unicast address.
The packaged configuration selects VirtIO. A unique match binds until reboot;
there is no fallback to another controller. Missing `net0` preserves settings;
`net0 = false` clears IPv4 settings while retaining the binding. Remote startup
looks up the configured candidate without binding it, then waits for the setup
owner to bind and assign an address.

`net0 = { driver = "virtio", dhcp = true }` requests a lease instead of static
address/prefix/gateway fields; the packaged profile uses this form. DHCP waits
for ready transport and reported carrier before acquisition; static settings do
not wait for carrier. Acquisition waits at most about ten seconds, then starts the
local session offline on failure.
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

From the interactive shell, `session bin://server.pxe` runs the endpoint example.
The server creates its receiver, launches two clients with caller grants, receives
both requests and replies in reverse order. `session bin://server.pxe --wide`
uses 4 KiB requests and replies with four file grants in each direction;
`session bin://server.pxe --abandon` closes one receipt so its caller sees
abandonment. `--saturate` retains sixteen receipts while a seventeenth client
reports queue saturation, then replies in reverse order. `--close` closes the
receiver after receiving a call and starts another client against a retained
caller grant; `--exit` lets process teardown close the receiver. The shell
delegates launch authority through `session`; endpoint creation is also
delegated to providers launched with `service start` or `service replace`.

`session bin://server.pxe --send` grants a client send-only authority. It sends
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

Add `--wait` to a server mode to observe its receiver and console input together:

```sh
session bin://server.pxe --wait --send
session bin://server.pxe --wait --delivered-timeout
```

Type a key at the initial prompt to start clients. Console input reports
`wait: console input`; receiver readiness is followed by RECEIVE and a message
kind report. The delivered-timeout mode leaves a ten-second window to type more
keys while waiting for cancellation. This option needs readable input and clock
grants. Wait timeouts resume waiting; input EOF or an input error stops the demo.
Readiness reserves no delivery: a queued CALL can expire before RECEIVE, which
still blocks until another message or endpoint closure. Modes without `--wait`
retain their ordinary blocking RECEIVE behavior.

`session bin://counter.pxe` runs a provider with two counter exports on one
receiver. A launched client receives different resource grants, attenuates a
copy and an IPC attachment, verifies protocol matching, and uses both CALL and
send-only delivery. The provider authenticates object ID and actual rights,
then acknowledges natural retirement. `session bin://counter.pxe --cut`
cuts an export during a delivered call, completes that call and denies later use,
acknowledges retirement while an old client handle remains open, and reuses the
same ID for a new export. `--exit` ends the provider with a delivered call; its
client reports closure after owner-process teardown.
`--queued-cut` parks the provider after the client sends a marker through
its export, then cuts while the client's following CALL waits in the queue;
the client reports DENIED before delivery. `--retire-full` fills all sixteen
ordinary delivery slots with raw SENDs, cuts an idle export, receives its
retirement notice ahead of those SENDs, acknowledges it and drains them.
`counter --forwarders bin counter.pxe textfs.pxe` uses two selected existing
native files to qualify directory child lineage, nested revocable copies,
ordinary COPY, explicit CUT and last-controller auto-cut. It also exercises
same-type service-slot retargeting, type/authority refusal, empty-slot NOT_FOUND,
explicit supervisor-controller takeover and target retention after all managers
close. Slot CUT remains terminal. This mode changes neither file and needs only
readable roots. These are manual qualification commands, not boot checks.

The standalone counter modes also accept `--wait` with the same interactive
start and mixed receiver/input wait:

```sh
session bin://counter.pxe --wait --cut
session bin://counter.pxe --wait --retire-full
```

CUT reports CALL and RETIRE; the full-queue mode reports RETIRE
before draining sixteen SENDs. Environment provider/client modes do not accept
this option.

`boot://init-install.pxe` is the install entry's boot init. It requires the
`disks` inventory service, read-only `boot_kernel` and `boot_archive` FILE grants
and the space factory, opens `boot://installer.pxe`, and creates the `install`
space with only that native program. The child receives these resources,
memory, a readable clock, random bytes and the read-only boot root; the kernel
adds the space's console, input devices, display and title grants. It receives
no launcher, tmp root or generic session handoff. Missing authority or an
unpackaged installer reports failure and stops; this entry does not supply an
installer stub. Init drops the factory, waits for completion and reports its
status on the Caelum tab.

[disk.h](include/disk.h) provides bounded native inventory, exclusive raw-disk
I/O, flush/release and read-only volume-open wrappers. Raw writes require a live
claim; flush supplies durability and release rescans GPT. Device alignment and
target bounds remain native checks. No wrapper retries an uncertain operation.

Boot init normally mounts volumes from its configuration. A shell holding
`native_mount` can still select a native volume with
`mount --partition 1 --volume system --read-only data://`, or request writable
content grants with `--read-write`. Exactly one access mode is required. Writable
roots require WRITE on the supplied `native_mount` authority and a writable
backend with understood filesystem features. `--optional` continues only when
`native_mount` is absent; failures from supplied authority stop the script.
Names are explicit directory bindings, separate from service namespace entries. Duplicate or conflicting names fail without replacement.
The shell reserves binding storage before opening and rechecks the service
namespace before publication; it closes an unpublished root on conflict.
The profile supports sixteen selected roots including boot, tmp and HOST.
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

`poweroff` and `reboot` are builtins of a shell holding the `power` resource.
Boot init grants it only to spaces that set `power = true` in their boot
configuration; the live Development and installed `pyxis` spaces do, as does the
rescue space. Session forwards it to local successors but not when starting
remote services, and the shell forwards it only to session successors.
The separate `remote_power = true` boot-space setting grants `remote_power`
through trusted session handoffs; the remote supervisor binds it as `power`
only in its root shell. Live/PXE Remote enables this; installed defaults do not.
The remote terminal is unauthenticated: any LAN peer reaching the live/PXE
terminal can reboot or power off the machine. The owner accepts this exposure
for a sole-user home LAN. Ordinary programs and services receive neither grant.
The kernel flushes native pools before powering off or restarting; a failure is reported and the
system stays up. [power.h](include/power.h) provides `power_off` and
`power_restart`.

[directory.h](include/directory.h) provides `directory_filesystem_info` through a
borrowed FILESYSTEM_INFO directory grant. Its bounded record supplies npfs
type, read-only and GPT/filesystem degraded flags, opaque pool/volume IDs,
volume name, the mounted pool's selected journal sequence as `generation`, and
shared-pool allocatable capacity in bytes. Capacity excludes the two header blocks
but includes metadata and reserves; equal pool IDs identify shared capacity that
must not be summed per binding or volume. Opening validates geometry and root
envelopes, not global allocation accounting. All record fields are available
on success; used/free bytes, charged bytes, guarantees, quotas and percentages
are unavailable and never implied zero. Queries preserve caller output on
failure, perform no whole-image check and acquire no additional authority.
Other directory backends return BAD_OPERATION.

Shell, session and remote handoffs forward the selected root list with queried
rights and transport masks. Providers requested with `--read-only` attenuate
both roots and cwd.
Read-only attenuation preserves held FILESYSTEM_INFO authority, including for
the HTTPS provider's native trust roots, without adding it to other grants.
The cwd chain is an explicit selection too: a launcher
selecting fewer roots must omit inherited cwd or select a safe cwd from that
subset. Display paths confer no authority. Mount resources remain in trusted
init and are excluded from child handoffs.

Each process owns one environment of named capabilities and text. Launchers stage
explicit entries and queried grant masks; ordinary programs retain their existing
allowlists. Trusted successors explicitly capture private configured sources and
service controllers. Other children receive exported service slots, selected
filesystem directories and text, with per-child stdio overrides. Relative paths
retain the independently owned cwd chain.

Shell assignments create private values: `title="two words"; echo "$title"`
assigns TEXT, then passes one argument. TEXT can concatenate with literals without
field splitting, globbing or reparsing. Single quotes and escaped `$` stay literal.
`copy=$clicks` copies a typed capability privately; `export copy=$clicks` or
`export copy` selects it for inheritance. A CAP expansion must occupy the whole
word, quoted or unquoted: `$copy` passes `copy://` with that captured entry.
Use braces for dotted names, as in `${clicks.control}`. A conflicting child binding
refuses launch; copy the value to another private name. Assignments are standalone
commands or `export` operands. Sequential `;` commands see earlier assignments.

`describe PATH` or `describe $VALUE` reports resource metadata.
`revoke ${NAME.control}` uses the explicitly named controller. Foreground external
commands support typed `<`, `>` and `>>`: `>` truncates output and `>>` appends;
a whole-word CAP target uses its held value. With a writable destination,
`cat < text://guide > tmp://guide` saves a readable provider file, and
`echo extra >> tmp://guide` appends to it.

`x=$(echo hello); cat $x` captures a typed capability and reads its content.
Inline `cat $(cat text://welcome)` passes a held CAP through child admission.
Eligible re-readable FILEs pass without copying their bytes or charging retention
again. Byte output is retained as a read-only FILE, with configured defaults of
64 MiB/value and 512 MiB/session shared by session successors and panes. Crossing
either limit binds an immutable TOO_LARGE failed value, never a successful prefix,
and suggests writing to a file. Failed inline capture prevents the outer launch.
A failure before evaluation or during publication preserves old bindings. Once
evaluation starts, command failure publishes its actual result and reports failure.

`guarded=$(revocable $clicks)` and `svc=$(service start bin://counter.pxe --provide)`
publish the primary and private `.control` companion atomically. Paired services
are leaf-only: delegating a launcher refuses before provider startup. Publication
failure stops and drains the unpublished provider. An optional observation is a
separate private `.status` entry. Companion collisions refuse the publication;
copying, exporting or unsetting the primary does not act on either companion.
Use `${guarded.control}` or `${x.status}` explicitly. A writable native FILE held
in `x` accepts `> $x` and `>> $x`; read-only captured FILEs refuse writes.
Named `service start NAME IMAGE` remains available for bootstrap services, and
private HTTP request/response handling retains its existing commands.

A provider launched through `service start` receives private endpoint creation
and a one-use publication CALL endpoint. The shell atomically publishes an
exported `NAME` slot and private `NAME.control`, then acknowledges startup.
Either name colliding with any existing entry rejects the complete publication.
Replacement retargets the same slot through its held controller; it does not
replace already opened FILE objects. For example:

```sh
service start clicks bin://counter.pxe --provide
counter --lookup clicks
counter --add clicks
counter --restrict clicks
counter --hold clicks &
service replace clicks bin://counter.pxe --provide
counter --lookup clicks
export other=$clicks
counter --lookup other
unset clicks
unset clicks.control
counter --lookup clicks
```

`--restrict` delegates READ alone and checks that ADD is denied. A held service
slot follows replacement, while a held FILE continues to describe its original
bytes. A typed copy retains the captured masks and lineage; it does not install
an alias watcher or copy a separate controller. `unset NAME` removes this process's
publication. Future jobs omit that entry; already launched children keep their
own entries. Closing a slot controller leaves its target retained until the
remaining slot holders release it.

The provider's publication CALL expires after ten seconds, but the shell can
remain waiting if a provider exits before sending it: RECEIVE cannot wait on
provider exit at the same time. A failed provider setup is reported independently
from publication or cleanup errors.

The compiler must include the Pyxis x87/SSE2 defaults and floating-point runtime
helpers (compiler-rt builtins). `mandelbrot` draws through a mapped display buffer using double
arithmetic. Hold arrows to pan, `=`/`+` and `-` to zoom, and Escape to return to
the TTY. It needs display, keyboard and clock grants. Libc supports floating-point
formatting and a small math subset; it does not provide a full libm.
Mandelbrot waits for geometry and keyboard readiness together, replaces its
mapping at a render checkpoint and redraws the retained view with the new aspect.
A failed replacement retains the old mapping until another resize.
Rendering continues without input focus; focus loss clears held controls.
Once the view is complete it idles until input or a resize needs another draw.

`mousetest` shows numbered buttons 1–32, both precise scroll axes and the kernel's
surface-local position, with a drawing pad for the left button and a custom system cursor.
Buttons 1/2/3 are left/right/middle. `SX`/`SY` show the latest scroll report in
120 units per detent; positive means viewport right/down. Vertical arrows follow
`SY`. The indicators expire after 500 ms and clear on reset, geometry or focus
changes. Direction policy has already been applied by the kernel.
`H` toggles cursor visibility, `D` restores the default image, `C` restores the
custom image and `W` warps to the surface center. Escape returns to the TTY.
`L` requests pointer lock or cancels it. The panel shows `FREE` when unlocked,
`LOCK` while locked, and `WAIT` when a request remains pending after refusal or
revocation. `DX`/`DY` show the latest relative device counts while locked; the
ordinary position stays at the kernel's parked coordinates. Super+Esc revokes
lock; a fresh surface click permits a pending request to try once again.
`L` while waiting cancels that request. Polling never requests lock again.
It needs display, keyboard, pointer and clock grants. Resizing replaces its
mapping and preserves the overlapping drawing; failure retains the old mapping.
Locked resize preserves relative counts and accepted buttons.
Its update loop keeps running without input focus, clearing held buttons on
focus, boundary and state changes and allowing the scroll indicators to expire.
Keyboard and pointer controls follow their own session's focus.

`httpfs.pxb` publishes prepared GET/POST response streams and legacy HTTP FILE
snapshots; `--https` selects verified HTTPS. Trusted startup declares private RAM
staging (16 MiB by default), with isolated npfs backing opt-in for large uploads.
Prepared request, response and status owners retain the existing private shell
HTTP registry and `@NAME` selectors until B8's HTTP migration. Reservation checks
TEXT names and can reuse a capability entry's name. The registry holds HTTP
operation state and independently owned facets; assignment, `export` and `unset`
operate on process environment values.

Configured boot sessions use `--start-services` to publish independent `http`
and `https` instances after selecting `DNS_SERVER`. HTTPS loads
`boot://share/ca-certificates/cacert.pem` before publication, with bounded entropy
initialization. Reported HTTPS setup failure leaves HTTP and local startup usable.
To augment public roots, restart or replace the HTTPS instance:

```
service replace --read-only https boot://httpfs.pxb --https --ca-bundle home://private-ca.pem
```

`--read-only` attenuates the launched provider's native filesystem roots and
working directories. HTTPS accepts only this read-only native trust authority
and receives only its declared environment grants. `service start --optional --read-only` continues only
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

Foreground pipelines preserve the first stage's console-input, keyboard and
pointer grants. A final stage with file/pipe stdin and console stdout also receives
named terminal input with READ alone, so pagers can read keys separately from
content. It receives no keyboard, pointer or interrupt-arming right through this
rule. Both ends reading the console compete for one input queue; use a file or a
producer that does not read console stdin with an interactive pager.

Before truncating any redirected output, the shell checks it against every
stage's file stdin, including inherited file input. An alias or an inconclusive
identity check refuses the launch; different known domains suffice even when
object IDs are unavailable, so HTTP/text snapshots can redirect to native files.
Use `cat < input > output` for this protection: files a program opens from its
arguments, as in `cat input > output`, are outside the check. Output creation can
precede rejection, and later resize or launch failures still have no rollback.

Libc `stat`/`lstat`/`fstat` retain type and size while reporting optional native
identity and modification time. Check `st_valid` with `STAT_DEV_VALID`,
`STAT_INO_VALID` and `STAT_MTIME_VALID` before reading `st_dev`, `st_ino` or
`st_mtim`; unavailable fields are not meaningful zero values. Keep open
descriptors while comparing identity. Provider byte snapshots report their
private domain without inventing object IDs or timestamps.

Libc output remains unbuffered by default. `setvbuf` selects full or line output
buffering before stream I/O, using caller storage until `fclose`, or allocated
storage when passed NULL. `setbuf(stream, NULL)` selects immediate output and
exact future input reads by that FILE. Full/line buffering on input-only streams
fails with `ENOTSUP`; existing shared input read-ahead remains descriptor owned.
`fflush` drains queued output without a durability promise and retains an
unconfirmed suffix after failure. Buffered `fwrite` counts bytes accepted into
FILE ownership, including retained bytes when a flush fails; check `ferror` and
`fflush` even for a full return count. A failed old queue drains before accepting
new bytes, including after `clearerr`; a failed drain accepts zero new bytes.
Unbuffered `fwrite` counts confirmed backend progress. Recover retained output
with `fflush`. `fclose` attempts flush and close, preserving the
first error. `fseek` flushes before moving; `ftell` includes queued output,
using the current held file size for pending append writes. Size-query failures
and positions beyond the `long` range fail without changing the queue or cursor.

Call `fflush` before mixing buffered FILE output with descriptor I/O or delegated
handles. `dup2` replaces the target descriptor while retaining its FILE
association and selected access. Pending target output rejects with `EBUSY`;
source validation/allocation failures preserve the target. A failed native
close invalidates the target and reports the error without a release retry.
FILE pushback/EOF are cleared on successful replacement; source position and
read-ahead remain shared. A selected input FILE redirected to a write-only
object subsequently reads with `EBADF`.

[pyxis/descriptor.h](libc/include/pyxis/descriptor.h) bridges libc descriptors
to borrowed native bindings and atomic handle adoption. Libc retains ownership
of handles, file positions and read-ahead. Its try I/O supports native pipes and
consoles; files use the standard descriptor APIs. Foreground bundles can request
`pipe` with `create` rights in their manifest. An unrequested grant is omitted;
an unavailable required grant prevents launch.

[pyxis/job.h](libc/include/pyxis/job.h) owns a single direct native child through
the caller's existing launcher. It uses explicit resource/stream selections and
owned cwd/environment snapshots, checks every final grant, and retains a
WAIT/TERMINATE observer through stage retirement. Environment/provider auditing
belongs to the trusted caller; no descendant launch, bundle expansion, capture,
deadline or implicit inheritance is added. See the header for ownership and
the uncertain-publication rule.

[pyxis/capture.h](libc/include/pyxis/capture.h) constructs FILE-compatible output
producers with process-local collectors using explicit session account authority.
It retains an absolute deadline, interim byte-mode polling/read limits and owned
result collection; handed-off values bypass byte limits.
[pyxis/stdio.h](libc/include/pyxis/stdio.h) transfers owned typed results or
restricted copies of complete unread sources. Single-source `cat` uses this
helper and ordinary destinations retain byte copying.
The trusted caller audits child authority and reachability.
It retains storage through process retirement and, optionally, a fresh root or
subgroup selected by the supplied LAUNCH-authorized launcher. The library keeps sole CONTROL;
the direct child receives no descendant launcher.
Unknown direct admission stays quarantined until caller/session teardown.

[retention.h](include/retention.h) provides account creation, status and immutable
failed FILE construction. The root `session.pxe` creates its account from
`retention.value_limit_bytes` and `retention.session_limit_bytes` in
`config/session.lua`; omitted settings default to 64 MiB/value and 512 MiB/session.
Accounting follows logical FILE length, including sparse holes. Trusted shell,
mux and Remote successors share the account; ordinary jobs inherit neither
account nor factory authority. Collected failed values retain their outcome and
remain available for owned take even when data operations fail.

[pyxis/pipeline.h](libc/include/pyxis/pipeline.h) publishes an explicit native
stage array atomically and pumps owned finite input, final stdout and combined
stderr concurrently through directional pipes. It returns every stage result,
input acceptance/closure and capped private captures, retaining endpoints and
observers through cleanup. Its optional fresh group uses the same binding-scoped
LAUNCH creation and sole-CONTROL policy; stages receive no descendant launcher.
Existing shell/Lua launch behavior is unchanged.

## License

Original Pyxis material is licensed under [MPL-2.0](LICENSE). See
[LICENSING.md](LICENSING.md) for scope and third-party exceptions.
