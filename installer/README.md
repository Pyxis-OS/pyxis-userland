# Native installer

`installer.pxe` runs in the trusted Install Pyxis space. It needs disk inventory,
immutable boot kernel/archive FILEs, read-only boot assets, private memory,
clock/randomness and SYSTEM_INFO plus its console streams. Trusted init also
delegates Caelum console WRITE as `installer_diagnostics` for log records;
ordinary programs never receive it. Normal shells lack
these installer grants. `installer-init.pxe` passes the selected action as
arguments; optional RESTART authority remains narrowed. Install runs `pkg check
base` before choosing a target and `pkg --yes install base` into the new pool's
`packages` volume after the pool stage, before the boot entries are published
(`packages.c`); it needs installer-init's HTTP providers and launcher.

The plain frontend and later TUI share `plan.h`: source assets, read-only
preparation, Review and one confirmed executor. Preparing, printing, cancellation
and dry-run never claim write access, persist replay, format or clean up.
Temporary native RO ESP views are inspected and retired before mutation. The only frontend apply call is `install_plan_execute`, after explicit
`wipe`/`append`/`update` consent. Fresh read-only revalidation checks consumed
inputs; one-use kernel grants then revalidate the exact reviewed GPT before
mutation. Stale plans refuse without amendment.

## Plans

The menu offers Install, Update or replay of a printed plan. After read-only
preparation, Review offers Apply (Enter), Print or Dry-run. Install retains its
Proceed / Read the room, marker vetoes and journal choice. Update retains healthy
installer-layout GPT, compatible pool opening metadata and an empty journal;
committed journals require an installed boot for recovery first.

`--plan` stops at printing; `--dry-run` stops without applying. `--apply` reads
one framed plan on standard input, reconstructs the same choices and displays
Review. `--apply --confirm=wipe` or `--apply --confirm=update` supplies explicit
non-interactive consent and skips the interactive restart offer. It grants no
rights. Human reports/progress use stderr; stdout is the canonical printed plan.
A caller can save these bytes using its own grants; the installer does not
create a plan file or acquire writable namespace authority.

`plan_record.h` defines data-only `pyxis-install-plan 1` ASCII framing through
`end\n`. Fields have fixed order, canonical unsigned decimal values and lowercase
hex IDs/revision bytes/hashes. Missing, duplicated, unknown or malformed fields,
unsupported layouts and oversized input refuse. Parsing stops at the end marker;
following bytes cannot execute or change that plan. Each plan is capped at 16 MiB.
Source hashes bind recorded choices and all immutable source assets;
target hashes bind successful raw read extents, consumed bytes and complete
partition/EFI-directory Review, excluding boot-scoped numeric device IDs.

Serialized apply resolves a uniquely observed validated existing GPT GUID plus
geometry and inspected state, never an enumeration number. Duplicate GUIDs
refuse. This is logical identity, not authenticated hardware identity. Blank or
otherwise unidentified targets can be printed/dry-run, but apply only through
the held target in the same interactive planning invocation. Fresh Install IDs
and journal choices are recorded once. All source/target changes require a new
plan and Review.

Planning extends the existing raw npfs decoder; opening even a read-only mounted
pool retains it until reboot and blocks write admission. Traversal validates
consumed allocation/mapping/inode/directory structure, handles sparse file holes
as zeros and refuses cycles/aliases, I/O, allocation and capacity failures. This
is bounded inspection, not fsck: raw scans bound each directory's decoded
records, including free slots, at 65,536.

## Execution

The boot archive is installed as built, the core archive of
docs/wip/pkg-upgrade.md (step 2 task 3); the installer copies no programs to the
pool. The executor preserves offline admission, claim/release order and explicit
sync. It prepares the pool's volumes and configuration, then publishes the boot
payloads. Its staging reserve credits no cleanup; it is not a guarantee against
all allocation limits. EFI verification syncs and reopens native FAT files,
compares held-object identity and hashes all bytes, without a cache-bypass or
physical-media/power-loss claim. Installs from before the core archive kept
programs in `bin/REVISION`; after publication, `bin_cleanup.c` removes those
except the known previous revision's, and an unknown previous skips it.

The shared whole-ESP writer generates exactly the installed normal and rescue
menu entries, with a three-second timeout and the selected disk binding. Live
installer and FAT32 tool entries are never imported. Update regenerates the menu
even for the same revision, removing extra entries from older installations.
This task provides no FAT mount or config editor. Failed writes are not retried
or rolled back. Failures direct the owner to live-media recovery.
Mounted verification retains the pool until reboot. Restart is offered only after
successful interactive apply, with the existing remove-medium reminder.

Shared observational records in `progress.h` feed a bounded in-place console
bar when totals are known, or activity without an invented percentage. Major
phases leave one final line; short checks remain silent. The observer redraws
at most 4 Hz, coalescing clock checks at 64 KiB / 32 items. A blocking call shows
a static marker. Non-console output leaves only final major/long lines.

Phase boundaries and coalesced counters go to `installer_diagnostics`; `-v`
prints those details locally instead of bars. Diagnostic and local-output
failure disable only their own destination, without changing confirmed I/O.
A failed Review/report prevents apply. END closes observation, never promises
success or durability. Human output is stderr; stdout remains plan data only.

Build with `make installer SDK=/path/to/sdk MBEDTLS_PREFIX=/path/to/mbedtls`.
Use the configured export's headers/libraries and native npfs codec archive;
there is no host libc, formatter, firmware updater or compiler-container change.

## Classic and coexistence ESP paths

Classic keeps its fixed EFI loader and `boot/{caelum.elf,initrd.cpio,revision}`
paths, plus the allowlisted `boot/limine/limine.conf`. Healthy ESPs are written
through native FAT files without formatting; a damaged/missing classic ESP
requires its separately reviewed one-use rebuild grant. A readable but unclean
ESP also requires that recovery route; read-only admission does not establish
writable health, and any readable foreign menu binding still refuses it. Program sync and
verification precede boot-payload publication. No classic attempt directories
are created until installer task 5 adds committed boot-binding evidence.

The explicit coexistence profile receives APPEND rather than ERASE or ESP
rebuild authority. `--coexist` selects that behaviour without adding grants.
New Install appends the reviewed native-pool slot first; Update opens its
existing selected pool. Every volume operation uses that actual GPT entry.
Coexistence stages only immutable `EFI/pyxis/REVISION/ATTEMPT/` payloads through
the shared stager and prints their saved manual entry after verified sync.
It edits no existing boot menu or firmware and never reports activation.
Review requires a supported clean FAT32 ESP; FAT12/FAT16 refuse without a
rebuild. Checked free clusters must cover the fresh attempt's independently
rounded payloads, ownership record and conservative directory allowance before
Apply is offered. Unknown or insufficient capacity refuses without target writes.
The final read-only observation is fingerprinted and revalidated before effects;
it is not a space reservation. Existing attempts are never credited or deleted.

Copies run outside the final publication interval. BEGIN is followed immediately
by CHECK; final held-object/hash verification and classic menu/revision changes
use the original non-extending 30-second lease. All staged references close
before root drainage, RETIRE and control close; END names the returned interval. Failure reports partial
effects without automatic retry, deletion or rollback.
