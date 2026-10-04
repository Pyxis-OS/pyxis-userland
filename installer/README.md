# Native installer

`installer.pxe` is launched by native `init-install.pxe` through the live
image's Install Pyxis entry. It takes no arguments. It requires the explicit
disk inventory, original kernel/archive FILE grants, read-only app assets,
private memory, console, clock and randomness. It receives no launcher, mount,
network, writable home or namespace authority.

The normal path requires a validated GPT and a nonempty npfs pool in every
recognized pool partition, with a regular root `SAFE_TO_WIPE` in every live
volume. Read the room also admits foreign, blank, partially marked and damaged
contents. Any readable nonempty pool with no markers vetoes the entire disk in
both modes. Mounted or retained pools, other raw claims and nonoperational or
read-only devices are excluded. Unsupported GPT features/capacity, I/O and
allocation failures fail closed.

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
whole archive and generated configuration under `boot`. Installed configuration
fills the packaged template with timeout zero and a disk-GUID-scoped normal
command line, and omits the installer entry. Fixed `init-installed` mounts
partition 2's system volume read-write as `system://`; home stays in RAM.

Success requires disk flush, explicit raw release/GPT rescan, FAT path lookup
and byte-for-byte source comparisons, then ordinary read-only pool reopening
and marker verification. Failed mutations are not retried or rolled back;
failure may leave a partially rebuilt disk. The verifier retains the pool until
reboot. Reinstall requires booting the live image again. Delete the installed
marker to mark the pool final.

Build with `make installer SDK=/path/to/sdk`. The SDK must export the pinned
npfs header and target `libnpfs-format.a`; this program supplies the two memory
symbols. It uses no host formatter, FAT service or host libc. Logical sector
sizes 512, 1024, 2048 and 4096 are supported; media must fit the fixed ESP and
pool. No USB/NVMe driver, firmware-variable update or recovery tool is added.
