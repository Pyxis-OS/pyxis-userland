# Configuration editor qualification

Installer task 4, 2026-10-13. Scratch-only delivery: installed startup and normal
Install/Update remain unchanged. No owner media, native ESP activation or live
Continuum application is qualified.

## Revisions and build

Functional baseline: Pyxis `2f2109fc`, read-only native FAT32 loading, unchanged
whole scratch-disk hash and `fsck.fat -n`. Earlier default launch/build checks are
recorded in #247 / #786. Publication qualification rebases onto Pyxis main
`3455b3b2` (#790) and userland main `e3c6ff9` (#249). Integrated parent checkpoint
`c611de69` uses the real public identity ABI/helper; no private header or fabricated
issuer evidence. The inherited ports pin is `4ed452df`.

Published builder `pyxis-llvm23.1.3-4c23a20`, LLVM 23.1.3, `make -j8 image
LOG_LEVEL=info`: full source builds passed with userland `98fc5f5` and `e56dabe`.
The latter fixes a missing-file replacement migration that incorrectly rendered
the ordinary default instead of its explicit replacement. Final records/comments
do not change that executable behaviour. Kernel, SDK, runtime and applications
were rebuilt together after the mount reply changed; no stale ABI artifacts.

## Interactive QEMU and debugger

QEMU 10.2.2, Q35, nested KVM/host CPU, four CPUs, 512 MiB, OVMF, modern VirtIO
boot/scratch disks and RNG, no NIC/USB. The full built kernel and initrd are used;
local initrd fixtures change only trusted scratch declarations and, for migration
cases, literal catalog data. Synthetic GPT identities identify disposable media.
The baseline ESP is 128 MiB on a 160 MiB disk; positive runs use a 40 MiB ESP on
an 80 MiB disk (first sector 2048, 81920 sectors, 512-byte blocks, two mirrored
FATs). The smaller fixture changes no lease or write-admission rule.

Trusted startup supplies scoped `EFI/pyxis/config` and `boot/limine` parents and
bound control through declared grants; ordinary scratch roots stay RO. Commands
are typed interactively. Plan data goes to RAM files, not to a global current-plan
slot. Read-only GDB inspects mount evidence, mutation and interval state, without
calling target functions. Host mtools/hash checks inspect closed scratch images.

| Check | Observed result |
| --- | --- |
| Confirmed `config edit`, explicit Adopt | Complete committed set verified. Unmarked UTC, tab width 4, TERM and DHCP values retained; absent boot defaults added. |
| `config validate` after publication | Complete committed ESP preview, shared whole-triple validation and merged provenance. |
| `config plan > home://plan`, then apply | Native staging, prepared/committed records, selected read-back and successful lease release. |
| Previous set and cleanup | Exact recorded hashes match retained files. Second publication removes only the three verified older recovery files; one current previous triple/catalog remains. |
| Replay stale after publication | Refused before staging; no automatic replanning. |
| RO print, independent RW apply | RO client prints a plan; separately granted RW client applies it. Concatenating `config\n` after the frame proves framing leaves final-confirmation input available. |
| Target/source/catalog mismatch frames | Each refused before writes; whole-disk hash unchanged across all three. |
| RO edit | Refused without administrative directory/control grants. |
| Prepared/malformed/hash-mismatched records | Whole-archive loader fallback, no mixed selection. |
| Unsupported executable Lua | Printed changes only; file untouched, no Lua execution. |
| Duplicate ownership markers | Printed changes only; file untouched. All five RO negative fixtures leave the whole-disk hash unchanged. |
| Required migration kept | Apply refused; unchanged whole-disk hash and unmutated mount in GDB. |
| Explicit migrations | Entry home→work preserves owner partition 3; EXINIT→VISUAL preserves owner value and unrelated environment keys; tab width replacement becomes 4. Marker-looking string bytes remain data. |
| Missing session file | Explicit replacement creates terminal tab width 4, rather than default 8; committed set and native clean shutdown pass. |
| Limine change | Only owned entry changes; foreign prefix/Fedora entry/suffix remain byte-identical. Recorded previous Limine file equals the original. |

Selected hashes and retained previous hashes are independently checked after VM
stop. Session and Limine bytes outside managed spans compare exactly before/after
migration. Boot foreign prefix/suffix and unrelated space are retained; an entry
move may insert a separator newline at its destination. Recovery is manual; no
prefix-based deletion, rollback, repair or automatic resume is exercised.

The RO negative image hash before/after is
`44037b6e3b7a90de2d8256d4c609bfc72ce8f9ed3b20fa475e36610bb9ee8966`.
The required-decline image hash before/after is
`dcd023dbbed3ef6bb35ecc78142556fd4c73939e8c7297c642c252607ba72d9c`.
Target/source/catalog refusals also match their immediate pre-run whole-disk hash.
Raw disks, screenshots and session logs stay local, outside the repository.

## Bounded failure and limits

The 128 MiB run (`98fc5f5`) reaches committed-record publication, then reports
phase 6 / TIMED_OUT at sync because the native 30-second lease expires. It stops,
releases the interval and retains named previous files without retry. The record
is visible and matches selected bytes, but its final flush was not confirmed;
this is filesystem-view evidence, not a durable-success claim. The 40 MiB runs
complete within the unchanged lease. Native-media timing and power-loss behaviour
are unqualified; large-volume preflight cost remains debt before task 5.

A clone stopped with QEMU Quit retains the expected dirty bit and RW admission
refuses it. Host `fsck.fat -a` clears only that bit on a disposable fixture before
reuse, preserving selected bytes. The cross-grant run instead ends through the
kernel Ctrl+Alt+Delete shutdown path: native power sealing succeeds and subsequent
`fsck.fat -n` reports no dirty-bit repair or structural errors.

Source review, rather than injected faults, covers unavailable identity validity,
changed issuer facts at each publication check, uncertain transport completion,
rescue bypass and cleanup-error retention. Conservative refusal remains for a new
implicit space entry colliding with handwritten explicit array indices and for
unsupported Limine header-renaming moves. Static machine checks do not establish
mounted-volume, timezone-asset or live-network availability. There are no new
self-tests, fault-injection hooks, boot automation or default kernel log lines.


## 2026-10-14 128 MiB publication after retained allocation proof

The earlier 128-MiB lease failure above describes the preflight implementation
at `98fc5f5`; it is superseded by this scratch result, not removed as rationale.
Publisher code `fb9b767d`, Pyxis `2d76c70c`, fs `c52ab584`, ports `2296a06c`,
lwIP `a1aadb91`; ordinary `make -j8 image LOG_LEVEL=info` passes with the existing
`pyxis-llvm23.1.3-e0801c2` compiler. No compiler-container rebuild.

The pristine original 160-MiB scratch disk has a 128-MiB GPT ESP, 512-byte
clusters, two mirrored FATs and 258,078 clusters. QEMU 10.2.2, Q35/nested
KVM/host CPU, four CPUs, 512 MiB RAM, OVMF, modern VirtIO boot/scratch/RNG,
no NIC/USB. The current archive uses only the existing private scratch grant
profile override; no shipped installed profile is activated. No real ESP writes.

Confirmed `config edit` applies three missing boot entries and explicitly adopts
four existing owner values. Read-only GDB observes exactly one successful
BEGIN → CHECK → END in the same process; CHECK starts with a delivered active
interval and no publication mutation intervenes. Its absolute 30-second deadline
is unchanged through END. CHECK uses 616 block admissions and 291 ms; BEGIN
through successful END takes 4.711 s including debugger/shared-host overhead.
All 30 observed interval operations succeed, including canonical swaps, sync,
committed verification and cleanup. This is a profiled nested-QEMU result, not
a native latency or power-loss guarantee.

`config validate` reports the complete committed ESP selection. After clean
physical-overlay shutdown, independent host extraction matches all four selected
sizes/hashes to the committed record and all three retained previous files to
pristine bytes. Limine remains byte-identical; adopted session/network values
and foreign spans are preserved. `fsck.fat -n` is clean (17 files, 27 allocated
clusters); both FAT copies are clean. All owned QEMU/GDB jobs stopped.

CHECK failure is source-reviewed: publication stops in EXCLUSIVE, skips swaps
and later effects, retains named staged recovery artifacts, and still attempts
END through the existing cleanup path without replacing the original failure.
No artificial failing CHECK or kernel allocation-failure injection was added.
Native ESP testing awaits explicit owner approval/presence; use the parent
FAT32 native run sheet.
