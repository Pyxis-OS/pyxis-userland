# Shared ESP payload staging

`esp_payload.h` stages and verifies kernel.elf, boot.arc and revision.txt, plus
optional entry.txt, in one freshly created EFI/pyxis/REVISION/ATTEMPT directory.
The plan-time path builder supplies the same layout without opening anything.
Revision is a portable ASCII FAT component of at most 63 bytes (not config);
attempt is exactly 32 lowercase hex digits. Collisions refuse, without retry.
Every payload is nonempty and at most 1 GiB. Target verification rounds
`FILE_READ_MAX_BYTES` down to the trusted OBSERVE device block size when that
block fits the transport (512-byte blocks use 3,584-byte requests). Larger
blocks keep the transport maximum. Source checks use `FILE_READ_MAX_BYTES`;
copy chunks also fit `FILE_WRITE_MAX_BYTES`, avoiding an extra tail write.
Sequential writes grow each fresh empty file; there is no full-file resize
whose single mutation plan would grow with the entire payload.

The trusted caller supplies borrowed native root/control capabilities, retained
OBSERVE admission identity/geometry, reviewed sources and SHA-256 digests. The
helper re-observes immutable target facts and lifecycle state; it also reopens
the control's root to compare held domain/object identity. It never accepts a
path as authority or substitutes another target. Stage requires native
LOOKUP|READ_FILES|CREATE|WRITE_FILES on the root and OPEN_ROOT|OBSERVE|WRITE on
the control. No REMOVE grant is needed. Verification traverses with read rights.
After a planning view is retired, the caller matches the newly admitted RW
instance to Review and supplies that instance's OBSERVE facts: its live domain
is not assumed to survive retirement/re-admission.

Sources use the existing install_source: either a native readable FILE or
caller-owned memory (the filtered archive). Sources live throughout stage;
the request and source descriptors stay unchanged until stage returns, and
expected sizes/hashes are frozen Review inputs. The caller initializes the
shared crypto context with hash_init and retains it through verification.
Sources are checked before namespace effects and again while copying. Each
file is synchronized, the ownership.record lists exact paths/sizes/hashes,
and volume sync precedes reopening and verification. This is filesystem-view
verification after confirmed sync, not a cache-bypassing or power-loss proof.

Stage returns an owned opaque result even on partial failure when allocated.
It retains the attempt and file references needed for object-ID comparison;
verification reopens canonical names and checks their identities and bytes.
The public record is descriptive evidence: staged_verified records stage-time
success, not a permanent attestation. Call verify again at the caller's final
publication boundary. Borrowed root/control must outlive close; original
sources are no longer needed after stage. Close releases handles/memory only.
Any failed mutation can leave artifacts, regardless of its error status. Stop,
retain the reported path/phase and inspect manually: no replacement, deletion,
automatic resume, rollback or repair is performed.

The caller owns admission, plan/consent, BEGIN/CHECK/END and interval deadlines,
retirement and later ownership-based cleanup. The helper neither acquires nor
renews an interval, and successful verification does not certify lease validity.
It never opens raw storage, formats, writes EFI/BOOT, changes Limine/config,
selects an active boot source or activates installed Update. Coexistence can
retain entry.txt as the exact printed entry; it is not installed automatically.


`boot_entry.h` shares the existing pure normal/rescue planner and read-only
legacy certifier with the coexistence caller. The planner accepts frozen menu
bytes, caller-certified previous paths (or no previous binding), reviewed new
paths and the disk GUID. Only exact owned entries backed by that previous
binding, or the exact fully certified legacy pair, can change. Missing or
ambiguous menus stay untouched and report activation pending; outside bytes
are preserved. Legacy certification hashes all three existing payloads and
checks the kernel, rescue archive and revision; failure clears all evidence.
Its borrowed RO root stays with the caller and no file handle escapes.

The pure manual renderer emits unmarked normal/rescue entries only. An optional
reviewed ESP GUID selects `guid(ESP_GUID):/`; otherwise it uses `boot():/`, per
[pinned Limine v12.9.3](https://github.com/Limine-Bootloader/Limine/blob/v12.9.3/CONFIG.md#paths).
This is entry text, never ownership, readiness or activation authority. The
planner's recognition remains the existing `boot():/` contract. The caller owns
target identity, Review, adoption consent and final revalidation/publication;
no additional durable boot-binding record is introduced here.
