# cp

`cp [--] source-file... destination` copies files through the caller's native
roots and working directory. A directory destination receives each source's
literal basename; multiple sources require an existing directory. Existing
destination files are replaced. There are no recursive or metadata options;
`--` permits option-looking names. Success is quiet. Ordinary source/copy
errors allow later operands to run with an aggregate failure status.

The destination parent is retained with LOOKUP, CREATE, WRITE_FILES and REMOVE
through reservation, publication and cleanup. No destination READ/ENUMERATE or
write grant on an existing destination file is needed. Source files use READ
alone, and exclusive sibling temporary files use WRITE alone. There is no
direct-truncation fallback for restricted grants or unsupported backends.

Each copy tries at most 64 `.cp-` plus 16-hex-digit candidates. It retries only
ALREADY_EXISTS, skips the final destination name, and uses no entropy/clock
grant. Copy storage is fixed at the smaller native read/write transfer limit;
native positive short reads/writes advance only confirmed progress. Copying is
bounded by the initial FILE_SIZE: later growth is ignored and premature EOF
fails. Concurrent writes are not snapshotted.

After a complete copy and successful handle closes, rename replaces the final
name through the same parent handle. Source/destination aliases remain safe
from truncation, but get a new destination object; older held handles keep the
old object. Other writers must leave the temporary file/name untouched until
the operation ends. Exclusive creation alone does not enforce that condition.

Before publication, a failed copy attempts to remove only its confirmed
reservation. Unknown WRITE progress does not change reservation ownership, so
one cleanup unlink is still attempted. Failed CREATE provides no confirmed
ownership and never triggers removal. Failed publication never retries or
removes either name, because the namespace outcome is unconfirmed. Unconfirmed
cleanup reports a possible temporary remainder. These unconfirmed outcomes stop
the batch; close errors fail and are never retried.

Interruption can leave a named temporary file. There is no automatic stale-file
cleanup or crash-durability promise; closing handles is not synchronization.
Use the normal `sync` command when durability is needed. No mode, owner or
timestamp metadata is copied.
