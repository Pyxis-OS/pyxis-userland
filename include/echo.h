#ifndef USERSPACE_ECHO_H
#define USERSPACE_ECHO_H

#include <abi/echo.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* Requires ECHO_RIGHT_SEND. Blocks until matched reply, error or monotonic
 * deadline. Clears reply on failure; see abi/echo.h for limits and ownership. */
enum call_status echo_exchange(handle_t echo, uint32_t destination,
    uint64_t deadline_ns, struct echo_reply *reply);

/* Inspect only the held factory's immutable scope; zero ID ends the list.
 * Derivation returns a new one-interface factory with subset operation rights.
 * Outputs are cleared on failure. No inventory/configuration right is added. */
enum call_status echo_factory_next_interface(handle_t authority, uint64_t after_id,
    uint64_t *interface_id);
enum call_status echo_factory_derive(handle_t authority, uint64_t interface_id,
    uint64_t rights, handle_t *derived);

#endif
