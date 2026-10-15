#ifndef USERSPACE_OUTPUT_H
#define USERSPACE_OUTPUT_H

#include <abi/output.h>
#include <abi/syscall.h>

/* Borrow session account capture authority. Success returns two owned handles. The collector
 * belongs to this process only; never copy, delegate or store it in a value. */
enum call_status output_create(handle_t account, struct output_create_reply *reply);
enum call_status output_status(handle_t collector, struct output_status_reply *reply);
/* Provider-free native FILE feature query before allocating transfer copies.
 * Ordinary FILEs and console/pipe/exported grants return BAD_OPERATION. */
enum call_status output_producer_status(handle_t producer, struct output_status_reply *reply);
/* Call only after independently confirmed producer job/subtree retirement. */
enum call_status output_seal(handle_t collector);
enum call_status output_cancel(handle_t collector);
/* Nonblocking, reserve-before-consume. Pre-commit slot/policy failures leave the
 * queued result owned by the collector. Success returns owned primary + optional
 * observation. Late BAD_BUFFER is mapped to OUTCOME_UNKNOWN: never retry it,
 * because publication may already have consumed and released the result. */
enum call_status output_take(handle_t collector, struct output_take_reply *reply);

/* Move explicitly owned grants together, preserving actual wrappers and masks.
 * Never pass a borrowed descriptor handle. Success clears both source handles.
 * OUTCOME_UNKNOWN also clears them: ownership is uncertain, never retry or
 * byte-copy fallback. Definite rejection preserves ownership. BAD_OPERATION is
 * fallback-safe; exported/non-FILE destinations return it before submission. */
enum call_status output_handoff(handle_t producer, struct output_handoff_request *owned);

#endif
