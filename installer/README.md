# Native installer

`installer.pxe` is the first process of the `install` space, which native
`init-install.pxe` creates through the live image's Install Pyxis entry. It
takes no arguments. It requires the explicit disk inventory, original
kernel/archive FILE grants, read-only boot assets, private memory, console,
clock, randomness and read-only SYSTEM_INFO. It receives no launcher, mount,
network, writable tmp or namespace authority. An optional `power` resource,
narrowed to the RESTART right, lets it offer a restart after the final
`installed` or `updated` message: only an empty line restarts, any other input
stays, and a failed call is reported with status 0 and manual-restart text.

The first screen offers Install and Update. Install retains the Proceed / Read
the room consent flow described below. Update requires healthy matching GPT
copies with exactly the installer's two partitions, writable-mount-compatible
npfs opening metadata, an empty selected journal and a live `system` volume.
One damaged pool header/control copy is accepted when the kernel accepts its
valid peer. A committed journal is refused without replay. No marker is needed.

Read-only FAT32 traversal supplies the installed boot configuration and revision
when available. Damaged or missing boot files remain eligible for rebuilding,
anchored by the healthy GPT and compatible pool; unreadable revision text shows
`unknown`. Actual I/O/allocation failures and readable configurations bound to
another disk refuse the candidate. The live revision comes from SYSTEM_INFO.
This checks opening compatibility and available boot identity, not whole-pool
integrity or complete bootability.

After the exact word `update`, inspection repeats under exclusive raw access and
checks that partition bounds and GUIDs still match. The shared fresh ESP writer
first clears and flushes the reserved area, including both FAT boot sectors.
It replaces EFI, kernel, archive, configuration and revision while boot geometry
remains invalid, flushes the complete replacement tree, then writes both boot
sectors. The final flush precedes raw release for GPT rescan. This keeps a
partly replaced tree rebuildable instead of recognizing stale identity files. FAT traversal and byte comparison verify all authored
boot files, followed by a read-only `system` root reopen. Only then is `updated`
reported. GPT and pool are never written, replayed or migrated by Update. An
interrupted update is recovered by booting live media again and choosing Update;
there is no fallback entry, rollback or retry of failed mutations. Read-only
verification retains the pool until reboot.

The normal path requires a validated GPT and a nonempty npfs pool in every
recognized pool partition, with a regular root `SAFE_TO_WIPE` in every live
volume. Read the room also admits foreign, blank, partially marked and damaged
contents. Any readable nonempty pool with no markers vetoes the entire disk in
both modes. Mounted or retained pools, other raw claims and nonoperational or
read-only devices are excluded. Unsupported GPT features/capacity, I/O and
allocation failures fail closed.

A zeroed first sector with no GPT header in either location is listed as blank.
This describes its partition metadata, not a scan or erasure of its contents;
blank disks remain eligible only under Read the room.

Consent reads use the codecs and a write-free committed-journal overlay. The
entire log is validated before overlay reads; each reread image must match its
validated checksum. Inspection follows root/marker paths, not every file or
global allocation ownership. The installer lists volume labels as escaped text,
asks for a journal size, then requires the exact word `wipe`. The journal field
starts with ceil(pool bytes / 128) rounded to MiB, between 8 MiB and 1 GiB;
an explicit choice may be 1–1024 MiB if metadata and bootstrap files fit.
After acquiring exclusive raw access it repeats consent inspection before
writing. It never persists journal replay before consent.

The new layout has a 512 MiB FAT32 ESP at 1 MiB, then an npfs pool extending
to the aligned end before backup GPT metadata. The fresh pool contains only
`system`, its root and an empty regular marker. Only allocated metadata and
FAT boot-file storage are initialized; this is not secure erasure.
The ESP contains Limine at `EFI/BOOT/BOOTX64.EFI` and the original kernel,
whole archive, generated configuration and `boot/revision` under `boot`.
The revision record is the kernel build revision followed by a newline. Installed configuration
fills the packaged template with a three-second timeout, a disk-GUID-scoped
normal command line and the rescue entry's, which adds `boot.default_config=1`,
and omits the installer entry and any global `default_entry`. Update treats
exactly this two-entry form as valid. Boot init then gives `init-installed`
partition 2's system and home volumes read-write as `system://` and `home://`;
`tmp://` stays in RAM. Install and Update create the home volume when it is
missing and never write into it.

Success requires disk flush, explicit raw release/GPT rescan, FAT path lookup
and byte-for-byte source comparisons, then ordinary read-only pool reopening
and marker verification. Failed mutations are not retried or rolled back;
failure may leave a partially rebuilt disk. The verifier retains the pool until
reboot. After a partial failure, boot the live image again and choose Read the
room to reinstall; the same consent vetoes still apply. Delete the installed
marker to mark the pool final.

Build with `make installer SDK=/path/to/sdk`. The SDK must export the pinned
npfs header, `MOUNT_NPFS_MIN_JOURNAL_IMAGES` in the mount ABI, and target
`libnpfs-format.a`; this program supplies the two memory
symbols. It uses no host formatter, FAT service or host libc. Logical sector
sizes 512 and 4096 are supported, matching kernel GPT rescan; media must fit the fixed ESP and
pool. No USB/NVMe driver, firmware-variable update or recovery tool is added.
