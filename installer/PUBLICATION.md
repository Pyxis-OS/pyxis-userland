# Owned ESP publication

Normal builds enable the integrated Classic Install/Update publisher and installed
configuration loader. `INSTALLER_PUBLICATION_SCRATCH=1` is the default build
setting; explicit `=0` retains the legacy development build. Coexist keeps its
immutable-payload/manual-entry path; complete Coexist Install/Update/completion
QEMU qualification and a separate activation record gate its integrated publisher.
The historical build-setting name also gates an explicit disposable-image frontend.

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
`--plan`, `--dry-run` and replay share the planner. No prior `config` invocation
is needed when revision defaults and safe pre-selected migrations suffice.
[Installation](../../docs/userland/installer.md) and
[Update](../../docs/userland/system-updates.md) describe target consent and programs.

## Review and consumed inputs

Preparation is read-only. It freezes issuer-bound ESP identity, configuration,
catalog, record and menu bytes, archive declarations, personal pool override and
recovery evidence. Temporary native views are drained and explicitly retired.
Review shows the active source, required/selected deltas, owner-file adoption,
pool migration, proposed boot attempt, retained recovery and capacity. Verbose
output and the printed plan expose detailed bytes/hashes and entries. Review
output failure refuses apply. A printed plan conveys no writer authority.

Without usable history, the planner prepares the revision's defaults. Required
safe changes are pre-selected; valid owner values stay Keep. A valid literal
`system://config/boot.lua` can supply reviewed volume/space migration when no
complete ESP set exists and no nonempty ESP boot file would be overwritten.
Its original bytes remain at the same pool path as inactive recovery. Existing
ESP owner bytes take precedence; conflicts are shown without a silent merge.
Unsupported Lua is neither executed nor rewritten. Unrecorded nonempty files
require explicit adoption. An ordinary plan with unresolved configuration choices
refuses before writes and requires a fresh configuration Review.

ESP selection publishes the complete `boot.lua`, `session.lua`, `network.lua`
and catalog at stable `EFI/pyxis/config/` paths. Reviewed whole-archive selection
leaves those files untouched and records archive activation, certifying no ESP
set or default provenance. Active source and retained inactive bytes are distinct
reviewed facts. Prepared/malformed records require explicit disposition.

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

Staging, retained current/previous payloads, pinned rescue, configuration recovery
and allocation overhead enter capacity admission without credit for later cleanup.
The final non-extending 30-second publication interval runs CHECK immediately
after BEGIN, then rechecks frozen inputs and fresh payload identity/content.
It publishes prepared then committed boot-bound readiness before switching only
confirmed Pyxis-owned normal/rescue Limine spans last, with synchronization and
readback. Exact legacy adoption additionally requires a disk-bound recipe and
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

## Installed configuration selection

The trusted early reader supplies evidence for the kernel/archive actually loaded.
Readiness must match their sizes/SHA-256, revisions and trusted archive catalog,
as well as the complete selected configuration/catalog. A pathname, revision string
or newly staged hash cannot substitute for loaded-byte identity. Archive mode
explicitly selects the whole archive set. Missing, prepared, malformed,
incompatible or mismatching evidence also selects the complete archive triple;
rescue always uses archive defaults, never previous configuration or the pool
override.

Freeze the whole selection before effects. Copy selected literal bytes into
private unlinked RAM FILEs, sync and close writers, then retire the ESP view.
Declared READ-only session/network snapshots pass through trusted startup;
ordinary children receive no live ESP references or administrative writer
resources. Catalog identity remains independent of kernel/program revision.

## Disposable scratch frontend and limits

Only an explicit trusted `--scratch-publication` invocation selects the separate
frontend for disposable images. It installs no program revision. Review actions,
source selection, owner-file adoption and per-change choices use highlighted
numbered lists: Up/Down or a number, then Enter. Show full plan exposes numbered
changes and hashes. Selecting Confirm publication additionally requires the typed
word `confirm`; completing a pending publication requires typed `complete` after
its Review. Cancel writes nothing. It can preview handoff without launching
installed startup and should not be used on native media.

Configuration files remain bounded at 1 MiB, completion records at 8 KiB and each
boot payload at 1 GiB. Native bounded FILE transfers and the existing 30-second
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
