# Configuration editor qualification

Installer task 4, 2026-10-13. This is an implementation checkpoint, not completed
publication qualification. The editor stays scratch-only; installed boot and
Install/Update are unchanged. Stable bound-ESP identity is an external dependency.

The prior functional baseline is Pyxis `2f2109fc`: existing native read-only FAT32
loading of five scratch configuration/foreign files, zero scratch writes,
unchanged whole-disk SHA-256 and `fsck.fat -n`. No timing claim is made.

## Build and inspected contract

Builder `pyxis-llvm23.1.3-4c23a20`, LLVM 23.1.3, `make -j8 config boot-init
session`, against the existing main SDK/ports development export. Public ABI and
runtime match userland main `b38b0e09`; private literal/schema/editor sources are
compiled from this branch. Compilation and diff checks passed. The default image
and exact submitted-head CI remain to be checked in the parent integration.

Source review checked literal admission without Lua execution, owned snapshots,
shared typed decoding/boot merge, explicit marker adoption, frozen-source plans,
one-frame input, target/hash refusal, native authority/domain checks and ordered
prepared/committed publication. Corrections found during integration: confirmation
must follow one frame without waiting for EOF; unchanged consumed Limine bytes
must also be rechecked; reviewed external owner edits must not be blocked by old
selected hashes; irrelevant optional Limine must not block a Lua-only plan.
These are inspected properties; source inspection is not write-path qualification.

## Interactive QEMU checkpoints

QEMU 10.2.2, Q35, nested KVM, host CPU, four CPUs, 512 MiB, fresh OVMF variable
store, modern VirtIO boot and scratch block devices, VirtIO RNG, no NIC/USB.
The kernel is the separately qualified pointer-default build `0fe0258c` on
Pyxis `6fea298a`; the ABI is unchanged by the subsequently incorporated main
`4a09711e`. A local fixture replaces only candidate boot init, session, the
config bundle and explicit scratch boot declarations. It is not described as
an exact-head/default-image build.

The scratch device is 160 MiB with a 128 MiB GPT ESP at blocks
2048..264191, 512-byte sectors. All identities are generated fixture identities,
not owner hardware. No native drive is touched. Inputs include empty boot
overrides, UTC/tab-width-4/session TERM owner settings, DHCP networking and a
foreign Limine entry. Trusted startup delegates scoped `EFI/pyxis/config` and
`boot/limine` parents through declared sources; normal scratch URI access is RO.

1. **RW view, missing history**, application checkpoint `3e2f5b7`:
   configured FAT admission and narrowed sources work; `config validate` reads
   the triple, reports archive-merged spaces and missing-record archive fallback.
   `config plan` displays six entry deltas; unknown/unmarked owner settings remain
   Keep, absent defaults are proposed. Missing issuer facts refuse a replayable
   frame and `config edit`. Read-only GDB shows the originating mount published,
   writable, `CALL_OK`, with no writer mutation, dirty ownership or intervals.
   Whole-disk SHA-256 remains
   `8546e815826917b25bbe329acfcde4786502e730c009bda848733fa8438e0d46`;
   extracted FAT `fsck.fat -n` passes.
2. **RO view, manually authored complete record**, checkpoint `d0924b7`:
   a fixture record binds the exact triple/current catalog hashes (no native
   publication claimed). `config validate` previews complete committed ESP
   selection. `config plan` distinguishes old defaults from owner edits, preserves
   current UTC/tab-width/environment values, materializes the proposed boot
   entries and successfully runs shared whole-triple validation. It still refuses
   replay without issuer facts. GDB confirms a published RO mount and `CALL_OK`.
   Whole-disk SHA-256 remains
   `ad0fcfa406bb0ba28d077a656dfc8e89cf1e62c158c6eda3c74d5493fb66b4b8`.

The first configured scratch boot exposed a pre-existing GPT readiness race:
DISKS enumeration can expose a device before its GPT GUID is ready, while boot
init previously returned NOT_FOUND immediately. The selected lookup now waits
up to ten seconds, sleeping one millisecond between snapshots and refusing
ambiguity. It does not guess an ordinal or return a match while another identity
is pending. The second boot admitted the same scratch selection successfully.
RAM/default boots do not enter this lookup.

All qualification QEMU/GDB processes above are stopped. Screenshots/logs/raw
fixture disks remain local build artifacts, outside the repository.

## Remaining admission and scratch checks

Epsilon supplies retained GPT GUID/type/attributes and device geometry through
bound MOUNT_OBSERVE, with explicit fact validity. Consume its published SDK and
rebase this dependency onto current userland main. The absence path is delivered;
no fabricated UUID, caller sidecar, raw DISK grant or unsafe publication bypass
fills the gap.

Then qualify the actual native write executor on scratch ESPs: authority refusal;
confirmed edit/adoption and outside-byte preservation; complete committed loading;
missing/prepared/mismatching record archive fallback; unsupported Lua/ambiguous
markers untouched; stale input/catalog/target refusal; per-choice migrations and
required decline; previous-set retention and verified cleanup. Report completed
phase and retained recovery on failure. Confirm ordinary default image boot and
exact-head CI before marking delivery complete. No native staging or installed
activation is authorized by this checkpoint.
