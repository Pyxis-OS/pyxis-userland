#ifndef LIBC_PYXIS_TERMINAL_H
#define LIBC_PYXIS_TERMINAL_H

#include <abi/console.h>
#include <abi/syscall.h>
#include <abi/wait.h>
#include <stdint.h>

struct pyxis_terminal_state;

#ifdef __cplusplus
extern "C" {
#endif

/* The supplied descriptor only, with no ambient terminal fallback. Return one
 * for CONSOLE, zero otherwise (errno identifies the rejection). */
int pyxis_terminal_is_terminal(int descriptor);

/* READ or WRITE authority; does not subscribe. Clears size on failure.
 * All int operations below return zero on success, -1 with errno on failure. */
int pyxis_terminal_get_size(int descriptor, struct console_size_reply *size);

/* Each open object has one private mode owner, shared by dup aliases. A state
 * snapshots whether that owner holds PASSTHROUGH, not global terminal modes or
 * another owner's grant. GetState acquires no raw grant and needs no READ.
 * Success allocates a state; failure sets *state to NULL. */
int pyxis_terminal_get_state(int descriptor, struct pyxis_terminal_state **state);

/* Snapshot the preceding state and ensure this owner holds one PASSTHROUGH
 * grant. Requires input READ even when already raw. CONSOLE reads already
 * return unechoed bytes; continue using read/stdio/descriptor input so libc
 * read-ahead remains visible. Ctrl+C is data while any raw grant exists.
 * A failed acquisition returns no snapshot and never retries uncertainty. */
int pyxis_terminal_make_raw(int descriptor, struct pyxis_terminal_state **previous);

/* Set this owner's raw hold to the snapshot. Repeated calls are idempotent;
 * nested saves restore in reverse order, out-of-order calls apply the selected
 * snapshot (last call wins). Does not consume state. The fd must still name
 * the saved open object; aliases qualify, replacements fail with EBADF and
 * have no effects. Restoring raw requires READ. Native CLOSE is attempted once;
 * a failed/uncertain release makes future mode calls fail, never replay it. */
int pyxis_terminal_restore(int descriptor, const struct pyxis_terminal_state *state);

/* NULL is safe. Frees snapshot storage only, without restoring. A snapshot
 * retains its open object's storage, not descriptor authority: final alias
 * close releases the owner's raw grant and native stream even if states live.
 * Exclude concurrent use of a state before freeing it. */
void pyxis_terminal_state_free(struct pyxis_terminal_state *state);

/* Observe READABLE, WRITABLE and/or RESIZED, with native rights/status/events.
 * Zero deadline polls; otherwise absolute monotonic ns, at most 30s ahead.
 * Pass SIZE's generation for RESIZED; re-query SIZE after notification.
 * Read-ahead counts as READABLE. An active descriptor reader can return BUSY;
 * this function never consumes bytes or loss notifications. WAIT_ERROR and
 * closure remain visible; use ordinary reads to distinguish loss/EOF/hangup.
 * Storage is held during native waits, without a mode/table/read lock. Close
 * may finish before the wait returns; closed/replaced fd returns BAD_HANDLE
 * without writing events. A native handle closed/reused after the precheck can
 * temporarily direct the wait to another object; the final identity check
 * rejects its result. That wait can last until the bounded deadline (30s),
 * as can a wait held alive by another owner. No subscription survives return; events are unchanged
 * on any failure. Readiness reserves nothing and may already be stale. */
enum call_status pyxis_terminal_wait(int descriptor, uint64_t interests,
    uint64_t generation, uint64_t deadline, uint64_t *events);

#ifdef __cplusplus
}
#endif

#endif
