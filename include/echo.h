#ifndef USERSPACE_ECHO_H
#define USERSPACE_ECHO_H

#include <abi/echo.h>
#include <abi/handle.h>
#include <abi/syscall.h>

/* Requires ECHO_RIGHT_SEND. Blocks until matched reply, error or monotonic
 * deadline. Clears reply on failure; see abi/echo.h for limits and ownership. */
enum call_status echo_exchange(handle_t echo, uint32_t destination,
    uint64_t deadline_ns, struct echo_reply *reply);

#endif
