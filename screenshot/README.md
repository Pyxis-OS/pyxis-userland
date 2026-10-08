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
