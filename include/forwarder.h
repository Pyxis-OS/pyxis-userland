#ifndef USERSPACE_FORWARDER_H
#define USERSPACE_FORWARDER_H

#include <abi/forwarder.h>
#include <abi/syscall.h>

/* Creation returns two owned handles or neither. SAME_RIGHTS requires zero
 * masks. Ordinary COPY, IPC and launch retain the holder's lineage. */
enum call_status forwarder_create(const struct forwarder_create *request,
    struct forwarder_reply *reply);
enum call_status forwarder_revocable(handle_t target, uint64_t rights,
    uint64_t transport, uint64_t flags, struct forwarder_reply *reply);
enum call_status forwarder_slot(handle_t target, uint64_t rights,
    uint64_t transport, uint64_t flags, struct forwarder_reply *reply);

/* CUT is terminal. Last controller close also cuts a revocable copy; a slot
 * keeps its target. Copy its controller explicitly for supervisor takeover.
 * An invalid RETARGET target empties a slot; later holder use is NOT_FOUND. */
enum call_status forwarder_cut(handle_t controller);
enum call_status forwarder_retarget(handle_t controller, handle_t target);

#endif
