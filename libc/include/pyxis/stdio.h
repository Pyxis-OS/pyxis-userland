#include <environment.h>
#ifndef LIBC_PYXIS_STDIO_H
#define LIBC_PYXIS_STDIO_H

#include <abi/startup.h>
#include <stdio.h>
#include <output.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow the current FILE owner's protocol/handle without changing indicators,
 * position, pushback, read-ahead or append policy. Zero includes NONE/invalid for absent or
 * closed standard streams. Null arguments fail with EINVAL; invalid FILEs or
 * broken descriptor associations fail with EBADF. Failure clears *binding
 * when nonnull. Success preserves errno and acquires no native reference.
 *
 * The handle remains borrowed until its descriptor closes or is replaced: never close it or
 * retain it past fclose/close/dup2 replacement. Delegation does not transfer the process's private
 * file cursor, read-ahead or pushback. Preserve append with the environment
 * companion when staging children. File-backed children start at offset zero;
 * pipe children cannot receive bytes already fetched into this process,
 * including the rest of a pipe value it has started reading. */
int pyxis_stdio_stream(FILE *stream, struct pyxis_stdio_binding *binding);

/* Attempt a complete unread READ-only, rereadable source (or held failure)
 * using owned restricted copies. The destination is a capture producer or a
 * pipe; a byte-only pipe returns BAD_OPERATION. One-pass successful streams keep byte copying.
 * Pending output, pushback, read-ahead and consumed input are ineligible.
 * The descriptor's borrowed handles are never consumed. BAD_OPERATION permits
 * ordinary byte copying; every other failure is final, including uncertainty.
 * The source stays open. This helper does not change stdio error indicators. */
enum call_status pyxis_stdio_handoff(FILE *destination, FILE *source);
/* Same dispatch for an explicitly owned typed result, including a failed OPEN;
 * no borrowed handles allowed. A capture destination follows output_handoff
 * ownership rules. A pipe destination moves only the value: success and
 * OUTCOME_UNKNOWN clear owned->value, a definite refusal keeps it, and the
 * observation always stays with the caller. */
enum call_status pyxis_stdio_handoff_owned(FILE *destination,
    struct output_handoff_request *owned);

/* Takes the next value from a value-capable input pipe instead of reading its
 * bytes: returns 1 with an owned READ-only *value at a value boundary, 0 when
 * bytes, end of input, pushback or a partly read value come next (and for any
 * other kind of input), or -1 with errno. Blocks like a read while the pipe is
 * empty. Byte reads after 0 continue the stream unchanged. */
int pyxis_input_take(FILE *stream, handle_t *value);

#ifdef __cplusplus
}
#endif

#endif
