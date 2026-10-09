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

Both directions stream, so memory stays constant: one 64 KiB block, one frame
and the SHA-256 state, whatever the file size. There is no fixed size limit; a
transfer is bounded by its declared 64-bit size, the destination's free space
and the reply deadlines. Sending reads the source twice. The first pass hashes
it before the metadata and digest are announced; the second sends it and hashes
it again, and a different digest or size fails before `end_data`, so a source
changed during the transfer is never published. Guest file names are valid UTF-8,
contain no ASCII control bytes or DEL, and are at most 200 bytes; host paths are
valid UTF-8 and at most 1024 bytes. Each
uncompressed chunk is at most 2048 bytes; encoded OSC frames are at most 4096
bytes. Confirmation and each peer reply have a 120-second deadline; cancellation
drains replies for at most five seconds. Unsupported or vanished peers therefore
fail without an indefinite wait. No other program should consume the named
terminal input during a transfer.

OSC 5113 uses the serialized kitty keys, with a mandatory `px_xfer=2`
negotiation and `sha256=HEX` file metadata. Stock kitty peers, and
`pyxis-remote` builds from another protocol revision, are refused. Compression,
deltas, multiple files, directories, links and resume are unsupported.

Data is windowed. A sender may have at most 64 KiB of file data unacknowledged,
and a receiver replies with a cumulative PROGRESS offset once 16 KiB has arrived
since its previous reply. Raw reads are buffered while data moves in either
direction: receiving reads data frames, sending reads the replies. Final
finish/cancellation acknowledgements use exact reads to preserve subsequent
shell input. Cancellation and failures use the same in-order handshake as
before, so at most one window of data in flight is discarded.

A receiver exclusively creates `.NAME.xfer-partial-ID` beside the destination
when data starts, and writes each decoded chunk to it through the 64 KiB block
while hashing it. Only after the declared size and SHA-256 both match does it
synchronize that file, atomically rename it with NO_REPLACE or explicit
replacement, and synchronize the directory. Unverified bytes reach the disk only
under the staging name. A full destination fails the write with `ENOSPC`. Handled failures and cancellation attempt to remove
only the staging name created by this transfer, and report cleanup failures.
Abrupt process or session death can leave this recognizable staging file,
holding a partial file of any size; stale staging names are never removed or
overwritten automatically. `ls` lists such dot names, and one is safe to remove
by hand once no transfer into that directory is running. Atomic
rename is the commit point: later cancellation or a failed acknowledgement
retains the completed destination. Native uncertain mutation outcomes are
reported and never retried automatically. The directory backend supplies the
atomicity/durability contract; a filesystem or authority that cannot perform
these native operations fails the transfer.
