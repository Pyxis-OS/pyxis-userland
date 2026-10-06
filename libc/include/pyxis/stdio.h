#ifndef LIBC_PYXIS_STDIO_H
#define LIBC_PYXIS_STDIO_H

#include <abi/startup.h>
#include <stdio.h>

/* Borrow the current FILE owner's protocol/handle without changing indicators,
 * position, pushback or read-ahead. Zero includes NONE/invalid for absent or
 * closed standard streams. Null arguments fail with EINVAL; invalid FILEs or
 * broken descriptor associations fail with EBADF. Failure clears *binding
 * when nonnull. Success preserves errno and acquires no native reference.
 *
 * The handle remains borrowed until its descriptor closes: never close it or
 * retain it past fclose/close. Delegation does not transfer the process's private
 * file cursor, read-ahead or pushback. File-backed children start at offset zero;
 * pipe children cannot receive bytes already fetched into this process. */
int pyxis_stdio_stream(FILE *stream, struct startup_stream *binding);

#endif
