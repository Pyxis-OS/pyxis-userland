# Native read-only FAT32 inspection

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
is no shell, configurable mount selector, installed binding or implicit URI mount.

At `fat32-mount>` use these commands:

- `ls /PATH` launches ordinary `ls` to enumerate native directory objects.
- `cat /PATH` launches ordinary `cat` to stream a native FILE.
- `info /PATH` prints available native SIZE/DOMAIN/OBJECT metadata. Directories
  may use a trailing slash. `info /PATH | /OTHER` holds both references during
  identity comparison, including long-name/short-alias comparisons. It reports
  unavailable identity rather than inferring it from a name or size.
- `read OFFSET LENGTH /PATH` performs one native FILE read at the decimal offset;
  length is 0–4096 bytes. It prints returned bytes and the actual count.
- `state` observes actual mount domain, device, GPT entry, block extent and flags.
- `close` closes the browser's root. `open` reopens the active bound instance;
  after successful retirement it admits the same fixed selection again.
- `retire` requests explicit retirement. A held root or remaining child/operation
  references produce BUSY, leaving the mount active. Use `close` first for a
  successful retirement. `quit` closes the root and requests retirement, reporting
  failure and retained state honestly before closing control/selection handles.

Paths are rooted in this volume with literal spaces and no quoting, expansion or
other URI schemes. ` | ` is reserved by the two-path info command. Commands and
paths are bounded by the 4096-byte input buffer. Files emit original bytes,
including terminal controls; inspect only known text at the console. FAT DOS
timestamps have no UTC offset and are not presented as native MTIME.

Keep media quiescent: the native mount excludes overlapping Pyxis mount/raw-write
admission but cannot prevent external mutation and supplies no immutable snapshot.
This profile writes, repairs and flushes nothing. Closing control alone does not
retire a mount. Process/group completion does not prove all mount references have
drained; failed exit retirement can retain bounded state until trusted reacquisition
and retirement, or reboot. Hardware ESP qualification is separate from QEMU.

Build against the matching exported SDK and runtime:

```sh
make SDK=/path/to/sdk init-fat32-mount ls cat
```

The Pyxis parent owns boot-menu entries, public ABI, SDK export and image assembly.
The internal `--browse-scratch`/`--browse-boot` modes are launched by this bootstrap
with attenuated trusted resources; command-line spelling grants no authority.
