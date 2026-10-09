# cp

`cp [-r] [--] source... destination` copies files through the caller's native
roots and working directory. A directory destination receives each source's
literal basename; multiple sources require an existing directory. Existing
destination files are replaced. `-r` also copies directory trees (below); there
are no metadata options, and `--` permits option-looking names. Success is quiet. Ordinary source/copy
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

## Recursive copy

With `-r`, a directory source is copied as a new tree: the root (`destination/<leaf>`
for an existing directory destination, otherwise the destination name itself) is
created exclusively, so an existing target of any kind fails that operand before
anything changes. Files are staged and renamed as above into directories created
first; empty directories are copied. File operands still replace.

Before copying data, cp creates a `.cp-tree-` plus 16 hex digit marker in the new
root and walks the source tree. Finding the marker means the destination lies inside
the source, since no object identity exists to compare; cp removes the marker and the
new root and fails. The walk also enforces the limits (32 levels below the root,
65,536 entries, 255-byte names) and rejects symbolic links, special and unknown
entries, so those failures normally occur before any file is copied. The copy pass
checks them again, because enumeration is live. An enumeration CHANGED outcome fails
the operand without restarting.

The first failure stops that operand and keeps what was copied; nothing is rolled back
or removed beyond cp's own confirmed temporary. cp reports the path and a summary of the
files and directories created. Later operands continue after an ordinary failure.
Traversal is iterative over a fixed stack (about 10 KiB) and uses no heap per entry.
Source directories need LOOKUP, ENUMERATE and READ_FILES; the destination needs the
rights above, which created directories request again.
