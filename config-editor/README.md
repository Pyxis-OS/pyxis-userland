# Configuration editor

Run `config validate`, `config plan`, `config edit`, or
`config apply [--confirm=config]`. Diagnostics and progress go to stderr;
`plan` and the editor's Print action emit framed data to stdout. Apply reads
that frame from stdin and requires final confirmation. No current-plan slot is
retained. A plan grants no authority, and changed target or source bytes refuse.

This delivery is for scratch ESPs. Installed Install/Update and boot selection
remain unchanged until installer task 5. Normal Install/Update never requires a
prior config invocation. The complete archive defaults remain available.

The bundle declares selected sources `config_parent`, `config_writer`,
`limine_parent`, `limine_writer`, and `config_control`. Trusted startup narrows
directory sources with their `path` to `EFI/pyxis/config` or `boot/limine`;
the mount source identifies that same admitted FAT32 instance. Read-only sources
allow validation/planning. Edit/apply need actual directory mutation and bound
MOUNT write/interval rights, stable issuer GPT facts, clock and random grants.
No disk, factory, call-site wrapper, or implicit ordinary-child grants are used.
Missing identity facts allow validation/printed diagnostics only.

Literal `return { ... }` configuration is admitted without executing Lua.
Unsupported handwritten Lua stays untouched. The revision catalog declares
entry IDs, defaults, disposition and optional data migrations. Managed entries
use actual `-- pyxis:begin ID` / `-- pyxis:end ID` line comments (Limine uses
`#`). Unmarked adoption and owner-value replacement require explicit choices;
outside bytes are preserved exactly. Missing history is unknown. The current
source catalog declares optional volume/space and session/network defaults;
it does not invent Limine paths or migrations. Network defaults now use the
ordered interface profile. Legacy `net0` entries fail shared static validation;
convert the owned profile explicitly using the [network migration reference](../net/README.md#private-profile-migration)
before planning/applying it. No implicit conversion or second network authority
is introduced.

Publication stages, synchronizes and read-verifies one complete Lua triple and
catalog; an exclusive bounded interval rechecks all consumed/staged bytes. A
successful BEGIN is followed immediately by one native full-volume CHECK in the
publisher process, before publication effects. A check failure stops publication
and releases the interval through the existing failure path; it does not retry
or renew the lease. A
prepared record is flushed before canonical swaps, then a committed record
names the exact selected hashes. One previous set and changed Limine recovery
are retained. FAT32 has no journal or multi-file atomic switch: any failure or
uncertainty stops without retry, rollback or resume and reports the reached
phase/artifact. Old recovery cleanup requires a matching recorded name/hash.
Limine recovery is manual. The loader freezes a complete committed triple or
uses the entire archive set, with rescue always using the archive.

Files are bounded at 1 MiB, literal depth at 32 and catalog entries at 128.
Static checks share startup validators and machine-setting rules. CPU presence
is checked when the declared read inventory is available. Mounted volume,
timezone-asset and live-network availability are reported as unchecked.
This command does not apply a configuration to Continuum or edit the separate
machine-settings store.

Task 5 extends the same record with boot binding for scratch installer
publication. Standalone editing preserves that binding and cannot implicitly
change archive selection into ESP selection or bind a new revision catalog to
an old boot pair. Such changes require a complete reviewed installer
publication. See `installer/PUBLICATION.md`; installed activation is still gated.
