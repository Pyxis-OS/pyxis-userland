# pkg

Manage immutable data, toolchain and command packages on the installed npfs packages volume.
Named repositories are literal data in system://config/packages.lua. Installation
uses the space's declared package management grant and GET-only HTTP providers.

```text
pkg status
pkg list [--available]
pkg check [REPO/]ID[@VERSION]
pkg [--timeout SECONDS] [--yes] install [REPO/]ID[@VERSION]...
pkg [--timeout SECONDS] [--yes] update [ID...]
pkg remove ID...
pkg verify
pkg reindex
pkg unlock
pkg upgrade [VERSION]
pkg repository list
pkg repository add [--unsigned] NAME URL
pkg repository remove NAME
```

Repository names follow the package ID rules. `REPO/ID` installs from one
repository; a plain ID offered by several gives a numbered choice on a console
and is refused elsewhere, listing the `REPO/ID` spellings. The review shows
which repository each fetched package comes from.

`pkg update` selects the current build of every installed package from its
recorded source. `pkg update ID...` selects only those installed IDs and any
dependencies their new manifests require; the whole resulting set is checked.
A source is a repository name bound to its configured URL and signed/unsigned
policy. Changing that identity requires an explicit console choice. Legacy set
members with no source adopt a sole compatible offering, shown as
`origin: NAME (only source)` in the review. Several possible sources are
presented in groups for one explicit choice; `--yes` never chooses one.
Unusable selected builds are proposed removals, each requiring separate console
approval, even with `--yes`. An unavailable dependency refuses without changing
the active set. If versions and origins are already current, update reports
`already up to date` and creates no new set.
If a newer offered dependency is blocked by another package's exact version
requirement, update names that constraint and reports `no compatible updates`
when nothing else can change; it never claims that offered version is current.

Repository reads fetch `current`, then its immutable `index/N.lua`. Signed
repositories also fetch the index signature and signer certificate, verified
against the pinned Pyxis Software Signing CA. Signed-index repository-name, accepted
serial/time/hash and first-contact signing-floor checks refuse stale or altered
indexes. There is no unsigned fallback: an unsigned development repository needs
`signed = false`; `pkg repository add --unsigned NAME URL` records that choice.

`pkg check` reads only the repository indexes and needs no packages volume. It
prints the build of ID that this system can install, or fails saying whether
builds exist only for other ABIs or not at all. Install runs it before its
Review. `--yes` switches to the reviewed set without asking, which lets Install
run pkg without a console; it does not answer a repository choice.

Program and toolchain packages record the ABI they were
built for; `pkg` reads what the running system provides from
`boot://os-release` (its release ABI list and its build revision) and offers, installs
and keeps only packages whose ABI it provides. Builds for other systems are
hidden and counted per ID. Stale stored builds refuse install, remove and
verify once, before any tree is hashed: one message lists every
stale package with its ABI and gives one `pkg install` and one `pkg remove`
command that clear them all.

Archive, member and installed-tree verification explicitly use positioned bulk
FILE reads with a heap buffer of at most 1 MiB. Every archive/inventory/member
hash, identity/size check and publication sync remains required. The backing
must support bulk READ (native RAM/archive/npfs or a completed httpfs snapshot);
a refusal is reported without silently retrying ordinary READ.

Install and remove hash only the packages they add or change; members
carried over unchanged are checked by their stored manifest, since stored trees
are immutable and were verified when installed. pkg verify hashes everything.

Changes review the proposed set before its one atomic current switch. Running
programs retain their views; new launches see the new set. A damaged current
requires repair or reinstall through rescue.

Install shows each package's phases: fetch, archive verification and extraction.
On a console every phase is one line redrawn in place and kept when it ends: fetch
shows the indexed size while the transfer runs and its measured size, time and rate
once the body is complete (the HTTP provider reports nothing before that), verify
shows bytes hashed of the archive, and extract shows files written of the manifest
count. When standard output is not a console, each phase prints plain lines once,
with no carriage returns. No remaining-time estimate is shown. A remote machine
session presents a console to the guest, so it receives the redrawn form.

Unlock is explicit recovery after a writer has stopped. It reports staging,
clears only the abandoned lock and retains incomplete attempts. Never use it to
cancel a live writer. Ctrl+C during download or preparation uses ordinary
foreground interruption and can leave this lock; cancel at Review releases it.
The confirmed switch is guarded until its actual result is known. Boot cleanup
removes unused versions before consumers start, retaining only the active set.
No archive scripts execute.

`pkg upgrade [VERSION]` reads only the signed `releases` repository and selects
the named release or its last published release, refusing the one already
running. It needs an installed system booted from its own boot entry and a space
with `package_upgrade = true`; live media and rescue boots refuse. Review shows
the release and installed packages whose ABI it does not provide. After archive
verification, the shared boot publisher stages a fresh attempt. Success requires
reboot to run the new
release; ordinary packages upgrade separately. Boot admission is single-use per
boot, so a failure from admission onward needs a reboot before retrying.

A package declares its command exports in package.lua. A plain export names a
.pxe path within tree; a bundle export names a .pxb directory and one declared
bundle command alias. Undeclared files are never discovered. Export names must
be unique across the selected set, with at most 256 total. Paths cannot traverse
another .pxb directory: its launch policy must remain attached to its commands.
Packages without commands export nothing.

Install and remove validate every declared target and publish the
sorted immutable sets/N.commands index before switching current. Plain targets
must be native P1F files. Bundle targets retain the complete manifest policy;
the selected alias and all executable entries are checked. Actual launch still
requires the bundle's external resource authority. pkg verify checks selected
files, targets and the exact command index.

For a pre-index installation, pkg reindex verifies the active package trees and
creates only its missing command index under the writer lock. It leaves current
unchanged and requires no repository access. It refuses
an existing index, including damaged or partial content; it never repairs one
silently. An empty selection with no current needs no index. New command views
refuse missing or damaged indexes for a nonzero active set.

Repository edits need package management and the separately declared confined
config writer. List needs only config read authority. URLs are exact distinct
HTTP/HTTPS bases without credentials, query or fragment; add accepts URLs
with or without a final / and appends it before storing or comparing. Duplicate
names or URLs refuse without changing configuration. At most eight bases of
2048 bytes each including the stored final /. Remove selects the repository by
name; an absent name reports not found. Missing configuration is empty; malformed
configuration refuses editing. Add uses the same published-index validation and
signature policy as later reads before saving. An unreachable host, HTTP failure
or invalid index refuses without
changing configuration; add it once reachable. Remove needs no network request.
Fetch failures name the full request
URL and retain the provider detail, including the HTTP status when available.
Edits share the package writer lock and atomically replace the strict literal
file. An uncertain publication retains the lock: inspect packages.lua before
explicit recovery with pkg unlock. A stopped repository writer can leave
packages.lua.new; unlock retains it, and the next successful edit replaces it. A live boot keeps edits only until reboot;
installed system edits persist after successful synchronization.
