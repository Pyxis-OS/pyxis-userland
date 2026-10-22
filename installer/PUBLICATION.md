# Owned ESP publication

Normal builds enable the integrated Classic Install/Update publisher.
`INSTALLER_PUBLICATION_SCRATCH=1` is the default build setting; explicit `=0`
retains the legacy development build. Coexist keeps its
immutable-payload/manual-entry path; complete Coexist Install/Update/completion
QEMU qualification and a separate activation record gate its integrated publisher.

The first action list offers Install, Update, Replay a printed plan and Cancel.
Only Install asks Classic/Coexist; Update discovers and revalidates the selected
layout and preserves that layout's authority/recovery limits. Discovery parses
media under a restricted read-only grant, reports refusal reasons and returns to
the action list on Back or failure. Ordinary media flow has no scratch prompt.
Classic recognition additionally requires the regular system-volume root file
`.pyxis-classic` with exact ASCII `PYXIS-CLASSIC 1` plus LF (16 bytes). New Classic
Install writes and syncs it; Coexist does not. Raw read-only discovery and child
revalidation check it alongside GPT/pool identity. A Classic-shaped unmarked
layout refuses without fallback to Coexist or a legacy-adoption option.
`--plan`, `--dry-run` and replay share the planner.
[Installation](../../docs/userland/installer.md) and
[Update](../../docs/userland/system-updates.md) describe target consent and programs.

## Shared publisher library

`boot_publisher.h` is the publisher's front-end interface, built with the modules
it uses into `libpublisher.a`. The installer links it, and `pkg upgrade` is to link
the same archive. The caller selects and admits the ESP, retires its planning
view, supplies the reviewed target facts, kernel and filtered archive sources,
revision, attempt and private RAM parent, and owns consent. The library prepares
the frozen review state, revalidates it inside the checked admission interval and
publishes: recovery, staging, the boot record, menu and retention as described below.
It opens no disk or mount itself. `installed_publication.c` is the installer's
adapter from its plan to these inputs.

## Review and consumed inputs

Preparation is read-only. It freezes issuer-bound ESP identity, the boot record
(`EFI/pyxis/config/config.record`), the owned menu and recovery evidence.
Temporary native views are drained and explicitly retired. Review shows the
proposed boot attempt, the retained previous attempt, recovery and capacity.
Review output failure refuses apply. A printed plan conveys no writer authority.
A malformed record, or one from before task 2b of
[package upgrades](../../docs/wip/pkg-upgrade.md), refuses: that machine
reinstalls (decision 28). A prepared record is an
[interrupted Update](../../docs/userland/installer.md#interrupted-update). The ESP holds no configuration; installed boot reads
`system://config/` on the pool.

The record holds the committed state, its transaction (the current attempt), the
current and previous boot bindings with each payload's path, size and SHA-256,
and the owned menu's previous and obsolete recovery copies.

## Ordering, activation and retention

An initial exclusive checked interval revalidates consumed inputs before program
writes. Programs and their revision manifest are copied, synchronized and verified
before the boot switch. Every publication stages fresh `kernel.elf`, `boot.arc`
and `revision.txt` under `EFI/pyxis/REVISION/ATTEMPT/` through the
[shared payload helper](ESP-PAYLOADS.md). Collisions refuse, including on
same-revision Update. Canonical-name, retained-object, full size/hash and immutable
ownership checks apply; names and paths alone confer no ownership.

Before activation, provision or fully reverify the small independent kernel/archive
pair under `EFI/pyxis/recovery/` and its static `/boot/limine.conf`. The recovery
archive is built from trusted rescue components, excluding personal/game/development
payloads. Normal publication never replaces or cleans this pair or fallback.
Foreign fallback bytes and higher-priority shadowing configurations refuse. The
fallback covers an absent primary `/boot/limine/limine.conf`, not a
corrupt-but-present primary. A fresh installation has this recovery path only
after the first complete provisioning is confirmed.

Staging, retained current/previous payloads, pinned rescue, the menu's recovery
copy, the staged record and allocation overhead enter capacity admission without credit for later cleanup.
The final non-extending 30-second publication interval runs CHECK immediately
after BEGIN, then rechecks frozen inputs and fresh payload identity/content.
It publishes the prepared then the committed boot record before switching only
confirmed Pyxis-owned Limine spans last, with synchronization and readback:
normal and rescue boot the new attempt, and `Pyxis OS (previous)` boots the
outgoing one, recorded as `previous_boot`. Pending completion and recovery own the replaced menu's previous entry,
whatever attempt it names, because that whole menu equals the digest-recorded
previous `limine.conf` (chronium, 2026-10-21). A committed menu without a
previous entry, as written before it existed, is still current; the next Update
adds it. Exact legacy adoption additionally requires a disk-bound recipe and
verified inherited kernel/archive/revision evidence rechecked within the interval.
Normal Update preserves the boot manager, unowned content and healthy filesystem;
ESP rebuilding is a separate explicitly reviewed Classic recovery grant.

In the Classic publisher, ambiguous ownership leaves the menu unchanged and prints the proposed entry:
**staged, boot activation pending**. A committed record alone is not successful
Update. Choose Update and the pending target to open a fresh completion Review,
select completion, then type `complete`. Exact recognized pending states reuse
the attempt, record and current/previous slots without a new retention generation.
Existing committed pending Coexist records use the same reachable explicit
completion and full proofs under their confined authority, without enabling new
Coexist publication. Edited or ambiguous menus, mismatching payloads and
unrecognized artifacts refuse. A power cut can leave FAT marked dirty; writable
admission then refuses even a recognized pending record. Completion requires a
clean ESP, after separately authorized external repair where necessary. The
publisher never clears the dirty bit or repairs the filesystem.

Retain current and one known previous boot attempt, plus pinned rescue. Older
attempts require confirmed activation and exact owned-manifest/hash evidence
before removal; unknown ownership skips cleanup. Program-tree retention remains
separate: same-revision `bin/REVISION` replacement is not atomic. Failure, deadline
or unknown completion stops at the reported phase with artifacts retained.
There is no automatic repair, retry or rollback, and no firmware-variable write.
Close is not sync or view retirement. Incomplete activation/verification does not
report `updated` or authorize ordinary program cleanup.

## Limits

The menu is bounded at 1 MiB, the boot record at 8 KiB and each boot payload at
1 GiB. Native bounded FILE transfers and the existing 30-second
interval remain unchanged. Battery below 40% or unreadable reported battery state
refuses; AC or no battery permits operation. Native internal-NVMe qualification
remains separate. No cross-file atomicity or power-loss durability is claimed.

The [qualification record](../../docs/development/experiments/installer-publication/README.md)
separates QEMU source/functional/cut evidence from the owner-reported 2026-10-20
native gate on vela. The native gate covered Classic clean Install, installed loading,
same-revision Update and rescue through the prior UI. New action/completion UI
checks are recorded separately, including the dirty-FAT refusal and completion
after external repair on disposable QEMU copies. Coexist activation is not qualified.
Foreign-file preservation was not checked on
that freshly wiped medium, and no native power-cut qualification is claimed.
