# Native FAT32 scratch writer

Build with an SDK exporting `pyxis_fs/fat32_write.h`, `libfat32-write.a` and
`libfat32-read.a`:

```sh
make -j16 SDK=/path/to/sdk init-fat32-write fat32-write
```

Use the trusted **Write scratch FAT32** boot entry. Its bootstrap creates
the `install` console space and asks for an explicit disk ID and one-based GPT
entry. Use disposable VirtIO media or qualified USB storage. This profile does
not qualify native ESP writes or NVMe coexistence. The existing read-only FAT32
Inspector and installer keep their separate entry points and behavior.

The RW open initially claims the whole disk. The bootstrap must successfully
release that claim before claiming the selected GPT partition. The child then
revalidates both GPT copies and FAT32 geometry, and confines all subsequent
reads and writes to the admitted partition. It receives disk INFO/READ/WRITE/RELEASE,
memory and console streams, with no DISKS service, MOUNT, launcher, roots or
namespace. WRITE also permits CLAIM: this is trusted code with callback and
active-claim confinement, not a permanently partition-attenuated capability.

At `fat32-write>` enter, for example:

```text
check
mkdir /SCRATCH
create /SCRATCH/Hello.txt = hello
append 2 =  world
read /SCRATCH/Hello.txt
replace 2 = replacement
extend 2 32
receipts
list /SCRATCH
quit
```

`mkdir` and successful `create` each issue a one-based session receipt number.
`append`, `replace` and `extend` require that number; existing files are never
adopted by name. `extend` sets a larger total file length, zero-filling new bytes.
Receipts and permission to update disappear when the session ends. Parent/name,
entry and complete chain provenance is revalidated before an update.

Create/append/replace use the first literal ` = ` to separate target and inline
payload. Payload is printable ASCII, with spaces preserved and no quoting,
escape expansion or added newline. An empty payload uses the delimiter followed
by no text. Paths may contain spaces, subject to the engine's ASCII mutation
profile. `info`, `check`, `list /PATH`, `read /PATH`, `receipts`, `help` and `quit`
are also available; read streams original bytes and list escapes controls.

Each mutation preflights whole-volume integrity, namespace, allocation, workspace
and work budgets before effects. Success is reported only after the engine's
final native flush. The session allows 32 receipts, 4096 bytes per file and eight
clusters per receipt, including directories. The command buffer is 4096 bytes;
workspace is at most 32 MiB, directory depth 64, with finite step/sector budgets.
Larger valid operations can return LIMIT. These are application bounds, not disk
format limits.

Uncertain writes/flushes permanently stop mutations. Recovery target, phase,
failed sector, complete staged/allocation/held-receipt chains and prepared-sector
count are printed; prepared workspace remains
untouched while the stopped session is open. Only info, receipts and quit remain
available. No rollback, replay, automatic cleanup or host repair is performed.
FAT32 is nonjournalled: interruptions can leave partial data, orphan clusters,
inconsistent FAT copies or uncertain names. Raw operations preserve dirty bits.

Exit explicitly releases the claim and reports failure honestly: RELEASE ends
the claim even when its flush fails, so failure does not retain exclusion or
establish durability. The existing device failure latch prevents further writes
until reboot after an uncertain device mutation. Final handle close also cleans
up a stopped/crashed process; close is not a durability receipt.
