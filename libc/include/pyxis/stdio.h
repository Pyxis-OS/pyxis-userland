#include <environment.h>
#ifndef LIBC_PYXIS_STDIO_H
#define LIBC_PYXIS_STDIO_H

#include <abi/startup.h>
#include <stdio.h>
#include <pyxis/response.h>
#include <output.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow the current FILE owner's protocol/handle without changing indicators,
 * position, pushback or read-ahead. Zero includes NONE/invalid for absent or
 * closed standard streams. Null arguments fail with EINVAL; invalid FILEs or
 * broken descriptor associations fail with EBADF. Failure clears *binding
 * when nonnull. Success preserves errno and acquires no native reference.
 *
 * The handle remains borrowed until its descriptor closes or is replaced: never close it or
 * retain it past fclose/close/dup2 replacement. Delegation does not transfer the process's private
 * file cursor, read-ahead or pushback. File-backed children start at offset zero;
 * pipe children cannot receive bytes already fetched into this process. */
int pyxis_stdio_stream(FILE *stream, struct pyxis_stdio_binding *binding);

/* Copy response metadata without changing indicators or position. Valid local
 * and inherited streams report absent metadata. EINVAL for NULL, EBADF for a
 * closed/invalid stream. Failure clears the output; success preserves errno. */
int pyxis_stdio_response(FILE *stream, struct pyxis_response_info *info);

/* Attempt a complete unread READ-only, rereadable source (or held failure)
 * using owned restricted copies. One-pass successful streams keep byte copying.
 * Pending output, pushback, read-ahead and consumed input are ineligible.
 * The descriptor's borrowed handles are never consumed. BAD_OPERATION permits
 * ordinary byte copying; every other failure is final, including uncertainty.
 * The source stays open. This helper does not change stdio error indicators. */
enum call_status pyxis_stdio_handoff(FILE *destination, FILE *source);
/* Same dispatch for an explicitly owned typed result, including a failed OPEN.
 * Sources follow output_handoff ownership rules; no borrowed handles allowed. */
enum call_status pyxis_stdio_handoff_owned(FILE *destination,
    struct output_handoff_request *owned);

#ifdef __cplusplus
}
#endif

#endif
