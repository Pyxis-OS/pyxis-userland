# Native FAT32 mount qualification

`init-fat32-mount` is a dedicated trusted bootstrap, separate from the raw engine
inspector, scratch writer and ordinary boot configuration. Build the image with
`INIT=boot://init-fat32-mount.pxe`. A boot entry with `boot.install=1` supplies
trusted `disks` authority and selects scratch mode, asking for an explicit disk ID
and one-based GPT entry. Without that authority the bootstrap uses only the initial
bootstrap's `esp_mount` issuer, fixed by the kernel to the executable's boot GPT
disk/ESP identity. `--boot` explicitly selects that mode even if DISKS is present.
The kernel's `--installed` and `--default-config` startup flags do not change this
authority-based selection. Missing selection or failed admission reports an error
without selecting other media; boot-ESP failure never falls back to scratch mode.

The bootstrap creates an inspection console space. It retains the selected
read-only DISK or boot issuer, bound mount control and root. Direct `ls`/`cat`
children receive memory, console streams and the sole read-only `esp` startup
root. They receive no disk, issuer, control, RETIRE or launcher authority. There
is no shell in the read-only profile, installed binding or implicit URI mount.

At `fat32-mount>` use these commands:

- `ls /PATH` launches ordinary `ls` to enumerate native directory objects.
- `cat /PATH` launches ordinary `cat` to stream a native FILE.
- `info /PATH` prints available native SIZE/DOMAIN/OBJECT metadata. Directories
  may use a trailing slash. `info /PATH | /OTHER` holds both references during
  identity comparison, including long-name/short-alias comparisons. It reports
  unavailable identity rather than inferring it from a name or size.
- `read OFFSET LENGTH /PATH` performs one native FILE read at the decimal offset;
  length is 0–4088 bytes. It prints returned bytes and the actual count.
- `state` observes mount domain, device, GPT entry, block extent, flags, interval,
  detached-object count and retained failure diagnostics.
- `close` closes the browser's root. `open` reopens the active bound instance;
  after successful retirement it admits the same fixed selection again.
- `retire` requests explicit retirement. A held root or remaining child/operation
  references produce BUSY, leaving the mount active. Use `close` first for a
  successful retirement. `quit` closes the root and requests retirement, reporting
  failure and retained state honestly before closing control/selection handles.

Paths are rooted in this volume with literal spaces and no quoting, expansion or
other URI schemes. ` | ` is reserved by two-path commands. Commands and
paths are bounded by the 4096-byte input buffer. Files emit original bytes,
including terminal controls; inspect only known text at the console. FAT DOS
timestamps have no UTC offset and are not presented as native MTIME.

Keep media quiescent: the native mount excludes overlapping Pyxis mount/raw-write
admission but cannot prevent external mutation and supplies no immutable snapshot.
The read-only profile writes, repairs and flushes nothing. Closing control alone
does not retire a mount. Process/group completion does not prove all mount references have
drained; failed exit retirement can retain bounded state until trusted reacquisition
and retirement, or reboot. Hardware ESP qualification is separate from QEMU.

Build against the matching exported SDK and runtime:

```sh
make SDK=/path/to/sdk init-fat32-mount init-fat32-mount-write shell ls cat
```

The Pyxis parent owns boot-menu entries, public ABI, SDK export and image assembly.
The internal `--browse-scratch`/`--browse-boot` modes are launched by this bootstrap
with attenuated trusted resources; command-line spelling grants no authority.

## Trusted mounted-write scratch profile

The packaged multicall alias `init-fat32-mount-write.pxe` selects scratch writes
when startup argv[0] is exactly that filename or its boot:// URI. The parent
boot menu selects `init=boot://init-fat32-mount-write.pxe boot.install=1`.
The alias is a copy of the same native binary and is installed normally. The
explicit `--scratch-write` option remains available through an existing launcher.
Both require install-mode DISKS; missing authority fails without boot-ESP fallback.
The bootstrap creates a trusted console selector (`--select-scratch-write`).
It asks for an explicit disk ID/GPT entry, opens that disk READ_WRITE, checks
ready/write/flush/error/GPT eligibility and RELEASEs its whole-disk raw claim
before mounted admission. Kernel backend qualification and admission policy
still apply. Use only disposable scratch VirtIO images for this task.

The selector launches `--browse-scratch-write ENTRY` with only the selected
DISK's INFO|READ|WRITE|RELEASE|MOUNT rights, read-only boot/tmp roots, memory,
launcher and console streams. The mounted writer receives no DISKS inventory.
It requests fixed READ_WRITE admission and exact DIRECTORY_CONTENT_RIGHTS;
boot-ESP issuer rights remain read-only. This utility is trusted: its selected
DISK WRITE right also permits raw CLAIM. It is not an editor/installed workflow.

The writable browser adds these native commands:

- `create /PATH` exclusively creates an empty regular file; `mkdir /PATH`
  creates a directory. `remove /PATH` removes a regular file; `rmdir /PATH`
  removes an empty directory.
- `rename /FROM | /TO` moves a regular file without replacement;
  `replace /FROM | /TO` permits replacing the destination regular file.
- `hold SLOT ro|rw /PATH` retains a FILE in an empty slot numbered 1–8.
  `hinfo SLOT` observes that held object. `hread SLOT OFFSET LENGTH` reads
  0–4088 bytes, including after unlink/replacement. `drop SLOT` closes it.
- `hwrite SLOT OFFSET = LITERAL` submits one literal byte payload through
  FILE WRITE; it prints the confirmed count without retrying a suffix.
  `resize SLOT SIZE` resizes the held object. Sizes and write extents must fit
  the FAT32 UINT32_MAX file-size bound; write payloads are at most 4080 bytes.
  Backend plan/capacity limits can still return LIMIT. Inputs are rejected,
  never clamped. Literal payloads have no quoting, escapes or implied newline.
- `hsync SLOT` calls FILE SYNC. `sync` calls the bound control's volume-wide
  SYNC. Neither clears the mount's owned dirty mark.
- `begin` requests a non-extending process-owned publication interval and prints
  its identifier/absolute deadline. `end ID` ends or acknowledges that exact
  interval. A child process does not inherit interval ownership. Other owners'
  mutations and sync return BUSY while ordinary reads remain available.
- `shell` launches the ordinary native shell with only read-only esp/boot/tmp
  roots, memory, streams and its existing launcher. Its working directory is
  esp://. It and its descendants receive no DISK, DISKS, MOUNT, controller or
  mutation rights. Direct `ls` and `cat` keep their original sole read-only esp
  root. Returning from the shell resumes the qualification browser.

Held FILEs survive namespace operations and root closure. `retire` remains BUSY
while roots, held FILEs, active intervals or deferred work remain; explicitly
`drop` each FILE, `end ID` and `close` before successful retirement. `open`
reopens the same active instance or readmits the fixed selection after successful
retirement. `quit` closes held FILEs, ends its known interval, closes its root and
requests retirement; failures report retained state and fail the process exit.
An uncertain mutation permanently freezes the mount, including data reads;
`state` remains available. No automatic replay, repair or forced retirement is
provided. Closing controller/selection handles does not erase failed state.

Successful native mutations complete their ordered flushes before returning.
Retirement clears only this mount's owned dirty mark after all data drains.
Clean power sealing belongs to the kernel; the utility adds no termination phase.
Physical ESP/USB/NVMe and clean-power qualification remain separate evidence.
