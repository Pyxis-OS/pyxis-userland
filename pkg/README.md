# pkg

Manage immutable data, toolchain and command packages on the installed npfs packages volume.
Repository URLs are literal data in system://config/packages.lua. Installation
uses the space's declared package management grant and GET-only HTTP providers.

pkg status
pkg list [--available]
pkg [--timeout SECONDS] install ID[@VERSION]...
pkg remove ID...
pkg rollback [SET]
pkg verify
pkg reindex
pkg unlock
pkg repository list
pkg repository add URL
pkg repository remove URL

Changes review the proposed set before its one atomic current switch. Running
programs retain their views; new launches see the new set. Rollback needs no
network. A damaged current requires an explicit verified set choice.

Unlock is explicit recovery after a writer has stopped. It reports staging,
clears only the abandoned lock and retains incomplete attempts. Never use it to
cancel a live writer. Ctrl+C during download or preparation uses ordinary
foreground interruption and can leave this lock; cancel at Review releases it.
The confirmed switch is guarded until its actual result is known. Boot cleanup
removes unused versions before consumers start, retaining the active and previous
sets. No archive scripts execute.

A package declares its command exports in package.lua. A plain export names a
.pxe path within tree; a bundle export names a .pxb directory and one declared
bundle command alias. Undeclared files are never discovered. Export names must
be unique across the selected set, with at most 256 total. Paths cannot traverse
another .pxb directory: its launch policy must remain attached to its commands.
Packages without commands export nothing.

Install, remove and rollback validate every declared target and publish the
sorted immutable sets/N.commands index before switching current. Plain targets
must be native P1F files. Bundle targets retain the complete manifest policy;
the selected alias and all executable entries are checked. Actual launch still
requires the bundle's external resource authority. pkg verify checks selected
files, targets and the exact command index.

For a pre-index installation, pkg reindex verifies the active package trees and
creates only its missing command index under the writer lock. It leaves current
and predecessor retention unchanged and requires no repository access. It refuses
an existing index, including damaged or partial content; it never repairs one
silently. An empty selection with no current needs no index. New command views
refuse missing or damaged indexes for a nonzero active set.

Repository edits need package management and the separately declared confined
config writer. List needs only config read authority. URLs are exact distinct
HTTP/HTTPS bases ending in /, without credentials, query or fragment; at most
eight bases of 2048 bytes each. Adding an existing URL changes nothing; removing
an absent URL reports not found. Missing configuration is empty; malformed
configuration refuses editing. No network request is made while editing.
Edits share the package writer lock and atomically replace the strict literal
file. An uncertain publication retains the lock: inspect packages.lua before
explicit recovery with pkg unlock. A stopped repository writer can leave
packages.lua.new; unlock retains it, and the next successful edit replaces it. A live boot keeps edits only until reboot;
installed system edits persist after successful synchronization.
