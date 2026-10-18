# screenshot

`screenshot PATH` captures the shown local screen as a non-interlaced RGB8 PNG.
It requires the named `screen_capture` resource and the output parent's LOOKUP,
CREATE, WRITE_FILES and REMOVE rights. It grants no extra path access.

Encoding reads and converts one native row at a time through libpyxis FILE I/O,
using conventional libpng and zlib with compression level 3. An exclusive sibling
`.screenshot-` name with 16 hexadecimal digits holds the output until encoding
and both FILE closes succeed; rename then replaces the destination. At most
64 names are considered. Failed creation/publication reports the possible names
without retry or uncertain cleanup; confirmed reservations are removed on
pre-publication failure. Other writers must leave the temporary name/file alone.
Abrupt termination can leave a temporary file; there is no stale-file removal.

Success is quiet and does not promise durability. Use `sync PATH PARENT` when
persistence matters. The existing remote workflow remains two guest commands:

```text
screenshot tmp://screen.png
xfer send tmp://screen.png
```

The host client needs `--download-dir`; its confirmation and 16 MiB limit apply.
See Pyxis's screen-capture and screenshot command references for authority,
consistency, failure ownership and qualification limits.

Print Screen uses the separate `screenshot-saver.pxe`. The initial session
consumes its private, space-bound notification grant and starts one saver with
only that grant, CAPTURE, the selected home directory, clock READ and runtime
memory authority. It receives no launcher or console streams. The ordinary
`screenshot PATH` command retains its existing replacement behavior.

The saver freezes the shown composition before doing storage work. UTC wall
time and the immutable space name form
`home://screenshots/SPACE-YYYY-MM-DDTHH-MM-SS.png`. The directory is created on
first use. A complete PNG is encoded in an exclusively created temporary file,
closed, then atomically renamed without replacement. Confirmed destination
collisions select `-2`, `-3`, etc., through `-1000`; exhausting that bounded
candidate set fails with LIMIT. The saver validates the request immediately
before each rename attempt. Definite failures remove the confirmed temporary;
OUTCOME_UNKNOWN and a failed reply copy preserve the uncertain names without
retrying or speculatively removing either candidate. Known partial temporaries
are removed on encoding failure. A failure never publishes a successful prefix. RAM-backed
home works with its usual reboot-loss semantics; no extra durability is promised.

The private request identity accompanies capture and completion. Switching the
shown space before capture cancels the request; after capture, saving stays in
the initiating space's home. Missing grants, missing wall time and storage
failures produce kernel-formatted feedback, rather than fallback authority.
Missing UTC wall time has its own "Screenshot failed: no clock" reason. The
initial session reports missing resources, a missing saver image and launch
failures to its existing stderr; the saver still receives no console authority.

Admission expires after 30 seconds. Validation cannot reserve or roll back a
filesystem rename: a rename already submitted when timeout wins may finish
late, so that timeout reports an unconfirmed save and asks the user to check
the directory. The saver never replays the request, ignores a denied stale
completion and re-establishes availability with NEXT.
