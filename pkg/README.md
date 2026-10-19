# pkg

Manage immutable data/toolchain packages on the installed npfs packages volume.
Repository URLs are literal data in system://config/packages.lua. Installation
uses the space's declared package management grant and GET-only HTTP providers.

pkg status
pkg list [--available]
pkg [--timeout SECONDS] install ID[@VERSION]...
pkg remove ID...
pkg rollback [SET]
pkg verify
pkg unlock

Changes review the proposed set before its one atomic current switch. Running
programs retain their views; new launches see the new set. Rollback needs no
network. A damaged current requires an explicit verified set choice.

Unlock is explicit recovery after a writer has stopped. It reports staging,
clears only the abandoned lock and retains incomplete attempts. Never use it to
cancel a live writer. Boot cleanup removes unused versions before consumers
start, retaining the active and previous sets. No archive scripts execute.
