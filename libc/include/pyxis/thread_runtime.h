#ifndef LIBC_PYXIS_THREAD_RUNTIME_H
#define LIBC_PYXIS_THREAD_RUNTIME_H

#include <abi/memory.h>
#include <abi/syscall.h>
#include <abi/thread.h>
#include <stddef.h>
#include <stdint.h>

struct libc_thread_context {
  struct memory_reservation reservation;
  uintptr_t fs_base;
};

typedef uint64_t libc_thread_key;

#ifdef __cplusplus
extern "C" {
#endif

/* Native-runtime bridge, not a C11/pthread profile. Prepare does not bind FS or
 * create a task. Each context owns a MEMORY copy and a fresh executable TLS
 * image. Pass reservation.identity/fs_base to native CREATE with a distinct
 * guarded stack. OUTCOME_UNKNOWN must not be retried automatically. */
enum call_status libc_thread_context_prepare(handle_t memory,
    struct libc_thread_context *context);

/* Caller-owned unbound context, or one whose native completion is known.
 * Keep this descriptor outside the context backing. Calling it on a running
 * sibling violates the precondition: BUSY protects native backing but does not
 * synchronize access to mutable runtime metadata. Once backing release commits,
 * the descriptor is cleared even if separate destructor-block or retained-handle
 * cleanup reports an error. Key-value retirement is best effort: CALL_OK does
 * not promise its physical backing was reclaimed. Failed/uncertain key backing
 * stays process-owned until teardown and is never retried by a collector. */
enum call_status libc_thread_context_release(handle_t memory,
    struct libc_thread_context *context);

/* completion must observe the native thread created with this context.
 * Wait for retirement (EXITED or STOPPED), then release context backing.
 * The observer remains caller-owned. The returned completion result stays valid
 * if cleanup fails; descriptor preservation follows release above. No stack
 * release is implied. */
enum call_status libc_thread_collect(handle_t memory, handle_t completion,
    struct libc_thread_context *context, struct thread_result *result);

/* Run this thread's language TLS destructors in reverse registration order,
 * then up to four key passes, then language destructors registered by keys,
 * then the registered runtime cleanup. Native EXIT must not return; refusal
 * stops the process. Recursive exit from a destructor stops the process instead
 * of reporting normal completion with an incomplete drain. It does not run
 * process exit handlers or release its live context. */
__attribute__((noreturn)) void libc_thread_exit(handle_t threads, uint64_t value);

/* Register one calling-thread runtime cleanup, run once after all language/key
 * drainage during orderly thread or initial-thread process exit. Repeating the
 * same callback succeeds; a different callback returns ALREADY_EXISTS. NULL or
 * registration after cleanup starts returns BAD_REQUEST. Earlier destructors
 * may register it. No allocation occurs. The callback must not initialize new
 * destructor-bearing TLS or rearm keys: no further destructor passes run.
 * Language-destructor registration after a registered runtime cleanup stops
 * the process: generated callers cannot reliably propagate its refusal. */
enum call_status libc_thread_cleanup_register(void (*cleanup)(void));

/* Process-local generational keys; values belong to the calling TCB. Deleting
 * a key neither frees values nor invokes destructors. A captured callback may
 * finish concurrently with deletion. Key callback order is unspecified; each
 * value is cleared before its callback, which may set it for the next pass.
 * Each pass snapshots eligible slots before callbacks; values set by callbacks
 * become eligible on a later pass. The registry grows using retained native
 * MEMORY independent of the creating TCB. Per-thread values allocate lazily:
 * unset get and NULL set do not allocate; non-NULL set may fail on allocation or
 * admission, preserving the previous value. Values retire during orderly exit
 * or confirmed stopped-context collection. Failed retirement of obsolete key
 * backing retains its actual charge until process teardown; it does not stop
 * the process or undo a published key/value. Uncertain addresses are forgotten,
 * never accessed or retried by the collector. These calls return native statuses. */
enum call_status libc_thread_key_create(void (*destructor)(void *), libc_thread_key *key);
enum call_status libc_thread_key_delete(libc_thread_key key);
enum call_status libc_thread_key_set(libc_thread_key key, void *value);
enum call_status libc_thread_key_get(libc_thread_key key, void **value);

#ifdef __cplusplus
}
#endif

#endif
