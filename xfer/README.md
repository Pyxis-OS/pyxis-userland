# xfer

Transfer one regular file through an interactive `pyxis-remote` terminal:

```sh
xfer send tmp://Example.class
xfer receive /home/user/Example.class Example.class
xfer receive --overwrite /home/user/Example.class Example.class
```

`send` saves into the host's explicit `--download-dir`, after host confirmation.
`receive` requests a host path and saves to one plain name in the inherited
working directory, after host confirmation. Host paths are absolute or relative
to the host user's home directory; there is no shell expansion. Existing guest
names fail at the final atomic rename unless `--overwrite` is supplied. Existing
host names always fail. Ctrl+C cancels; Escape remains part of the OSC decoder.

Build with the selected SDK and ports development export:

```sh
make -j16 xfer SDK=/path/to/sdk MBEDTLS_PREFIX=/path/to/ports-dev/mbedtls
```

The program borrows named `input` and `output` console grants independently of
stdin/stdout, holds libterm passthrough, and reads raw terminal bytes. It also
requires readable `clock` and `random` grants: the configured Mbed TLS PSA
initialization seeds its RNG before hash operations, with a two-second native
entropy deadline. Receiving requires CREATE, REMOVE and WRITE_FILES on the
inherited current-directory grant. Staging and publication use native file and
directory calls, since ordinary libc creation cannot exclusively create a name
or select NO_REPLACE. Source reads use libc open/fstat/read and reject nonregular
objects. SHA-256 uses the configured Mbed TLS PSA implementation from the ports
export, without vendored cryptographic code or an additional runtime library.

Both directions capture the complete file in memory, limited to 16 MiB. Source
capture finishes before metadata and its digest are sent; a size change while
capturing fails. Same-size concurrent edits can affect the captured bytes, whose
digest still describes exactly what is sent. Guest file names are valid UTF-8
and at most 200 bytes; host paths are valid UTF-8 and at most 1024 bytes. Each
uncompressed chunk is at most 2048 bytes; encoded OSC frames are at most 4096
bytes. Confirmation and each peer reply have a 120-second deadline; cancellation
drains replies for at most five seconds. Unsupported or vanished peers therefore
fail without an indefinite wait. No other program should consume the named
terminal input during a transfer.

OSC 5113 uses the serialized kitty keys, with a mandatory `px_sha256=1`
negotiation and `sha256=HEX` file metadata. Stock kitty peers are refused because
they do not implement this extension. Compression, deltas, multiple files,
directories, links and resume are unsupported. Receive-side raw reads are
buffered while the host waits for per-chunk PROGRESS; final finish/cancellation
acknowledgements use exact reads to preserve subsequent shell input.

A receiver checks the complete size and SHA-256 before exclusively creating
`.NAME.xfer-partial-ID` beside the destination. It writes and synchronizes that
file, atomically renames with NO_REPLACE or explicit replacement, then
synchronizes the directory. Handled failures and cancellation attempt to remove
only the staging name created by this transfer, and report cleanup failures.
Abrupt process or session death can leave this recognizable staging file;
stale staging names are never removed or overwritten automatically. Atomic
rename is the commit point: later cancellation or a failed acknowledgement
retains the completed destination. Native uncertain mutation outcomes are
reported and never retried automatically. The directory backend supplies the
atomicity/durability contract; a filesystem or authority that cannot perform
these native operations fails the transfer.
