# Native FAT32 inspector

Build with an SDK exporting `pyxis_fs/fat32.h` and `libfat32-read.a`:

```sh
make -j16 SDK=/path/to/sdk init-fat32-inspect fat32-inspect
```

Use the trusted **Inspect FAT32 (read-only)** boot entry. Its bootstrap creates the install
console space, lists disks and asks for an explicit disk ID and a one-based GPT entry.
Keep the media quiescent until inspection exits. The selected disk is
opened read-only. The inspector receives exactly INFO/READ disk authority,
memory and native console streams, with no disk service, launcher or roots.
The ordinary boot profile supplies no disk inventory authority.

At `fat32>` enter `info`, `check`, `list /PATH`, `read /PATH` or `quit`.
Paths use ASCII names or displayed ASCII short aliases; spaces are literal path
bytes, with no quoting or expansion. Listing preserves validated UTF-8 names,
escaping ASCII controls and backslashes as `\xNN` and C1 controls as `\u00NN`.
Only supported ASCII short aliases are printed; reading streams the original file bytes.
Errors are reported explicitly. Admission validates GPT and FAT32 geometry;
`check` also checks whole-volume allocation and reachability. No command writes,
repairs or mounts the media.

Only the shared engine's GPT/FAT32 profile and ready disks with 512-byte logical
blocks are supported. Every native read is at most 4096 bytes; after admission
the adapter also confines reads to the selected partition. The media must remain
unchanged throughout inspection: read-only opening provides no snapshot or writer
exclusion. Each operation has finite work and sector budgets and a directory
depth limit of 64. Workspace sizing uses the admitted geometry and refuses more
than 32 MiB; these application limits can report `LIMIT` on larger valid media.
